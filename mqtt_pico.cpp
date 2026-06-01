#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "pico/bootrom.h"
#include <string.h>
#include "hardware/adc.h"
#include "bsp/board.h"

#define END_MQTT "44.232.241.40" // emqx broker mqtt

#define ROOT_TOPIC "/Kevin/240025236"

#define TEMPERATURE_TOPIC ROOT_TOPIC "/temperatura"
#define LED_TOPIC ROOT_TOPIC "/led"

// Como o sensor gera tensão elétrica/sinais analógicos, é necessário utilizar o ADC para converter para número digital
void inicializar_sensor_temperatura();

// Caso ocorra uma falha ao conectar com Wi-Fi, o LED da placa piscará uma vez
// Caso o Wi-Fi seja conectado com sucesso, o LED da placa piscará duas vezes
void sinalizar_erro_ao_tentar_conectar_com_wifi();
void sinalizar_wifi_conectado_com_sucesso();
bool configurar_wifi();

float ler_temperatura();
bool mensagem_inteiro_positivo(const char *texto, int *valor_saida);

// Callback (Evento) que sinaliza o momento em que o broker MQTT aceita a conexão do cliente (placa)
static void mqtt_conectado_cb(
    mqtt_client_t *cliente,         // Estrutura que representa o cliente MQTT conectado ao broker
    void *arg,                      // Permite receber dados genéricos definidos pelo usuário
    mqtt_connection_status_t status // Contém o status da conexão MQTT
);

// Callback (Evento) utilizada para realizar a leitura dos dados recebidos através dos tópicos MQTT
static void mqtt_dados_recebidos_cb(
    void *arg,            // Permite receber dados genéricos definidos pelo usuário
    const uint8_t *dados, // Buffer contendo os dados recebidos em formato texto/bytes
    uint16_t tamanho,     // Tamanho dos dados recebidos
    uint8_t flags         // Bits de controle da mensagem MQTT
);

// Callback (Evento) executado quando uma nova publicação MQTT é recebida, informando o tópico associado à mensagem
static void mqtt_recebendo_publicacao_cb(
    void *arg,          // Permite receber dados genéricos definidos pelo usuário
    const char *topico, // Nome do tópico MQTT que recebeu a mensagem
    uint32_t tamanho    // Tamanho do nome do tópico
);

// Callback executado quando uma operação MQTT é concluída, informando sucesso ou erro da requisição
static void mqtt_requisicao_cb(
    void *arg,  // Permite receber dados genéricos definidos pelo usuário
    err_t error // Sinaliza sucesso ou erro da requisição MQTT
);

// Estrutura para armazenar as informações do cliente MQTT
struct mqtt_connect_client_info_t info_cliente = {
    "",    // Client ID (identificador do cliente MQTT)
    NULL, // Usuário de autenticação MQTT
    NULL, // Senha de identificação MQTT
    0,    // Tempo de keep alive
    NULL, // Tópico do Ultimo desejo
    NULL, // Mensagem do Ultimo desejo
    0,    // Quality Of Service do último desejo
    0     // Último Desejo Retentivo
};

bool conectar_com_servidor_mqtt();

// volatile previne que o compilador faça otimizações perigosas
volatile bool led_estado = false;
volatile bool timer_ativo = false;
volatile uint32_t intervalo_led_ms = 0;
volatile uint32_t ultimo_toggle = 0;

static bool connectado_com_sucesso = false;
static mqtt_client_t *mqtt_cliente;

int main() {
    stdio_init_all();
    inicializar_sensor_temperatura();

    if (!configurar_wifi()) {
        return -1;
    }

    if (!conectar_com_servidor_mqtt()) {
        return -1;
    }

    while (true) {
        cyw43_arch_poll();

        if (timer_ativo) {
            uint32_t agora = to_ms_since_boot(get_absolute_time());

            if((agora - ultimo_toggle) >= intervalo_led_ms) {
                ultimo_toggle = agora;
                led_estado = !led_estado;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_estado);
            }
        }

        if (board_button_read()) {
            if (connectado_com_sucesso) {
                float temperatura = ler_temperatura();
                char mensagem[50];

                sprintf(mensagem, "Temperatura: %.2f C", temperatura);
                printf("%s\n", mensagem);

                mqtt_publish(
                    mqtt_cliente,
                    TEMPERATURE_TOPIC,
                    mensagem,
                    strlen(mensagem),
                    0,
                    0,
                    mqtt_requisicao_cb,
                    NULL
                );
            }
            sleep_ms(2000);
        }
        sleep_ms(10);
    }

}

static void mqtt_conectado_cb(mqtt_client_t *cliente, void *arg, mqtt_connection_status_t status) {
    printf("Cliente MQTT conectado com o status %d\n", status);

    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("Conexão MQTT aceita\n");
        connectado_com_sucesso = true;
        mqtt_sub_unsub(
            cliente,
            LED_TOPIC,
            0,
            mqtt_requisicao_cb,
            NULL,
            1
        );
    }
}

static void mqtt_requisicao_cb(void *arg, err_t error){
    printf("recebendo requisição %d\n", error);
}

static void mqtt_recebendo_publicacao_cb(void *arg, const char *topico, uint32_t tamanho){
    printf("recebendo mensagem no topico %s\n", topico);
}

static void mqtt_dados_recebidos_cb(void *arg, const uint8_t *dados, uint16_t tamanho, uint8_t flags) {
    printf("Recebido: %.*s\n", tamanho, dados);

    char mensagem[20];
    
    // Verificar se a mensagem cabe no buffer
    if (tamanho >= sizeof(mensagem)) {
        printf("Mensagem muito grande\n");
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        return;
    }

    // Copia os bytes recebidos pelo MQTT para o buffer mensagem
    memcpy(mensagem, dados, tamanho);
    // Adiciona o terminador de string para permitir operações com texto
    mensagem[tamanho] = '\0';
    int intervalo_segundos;

    if(mensagem_inteiro_positivo(mensagem, &intervalo_segundos)) {
        // Configura o intervalo do LED
        intervalo_led_ms = intervalo_segundos * 1000;
        timer_ativo = true;
        printf("Intervalo configurado: %d segundos\n", intervalo_segundos);
        return;
    }
    printf("Valor inválido\n");

    timer_ativo = false;
    led_estado = false;

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

float ler_temperatura() {
    uint16_t valor = adc_read();
    
    // 3.3 -> Tensão máxima que o ADC consegue medir
    // 1 << 12 -> Bit Shift equivalente a 2^12 = 4096
    // Utilizado para converter a leitura digital do ADC -> tensão
    const float conversao = 3.3f / (1 << 12);
    float tensao = valor * conversao;
    // Fórmula vem do datasheet oficial do RP2040, que converte a tensão do sensor -> graus Célsius
    float temperatura = 27.0f - (tensao - 0.706f) / 0.001721f;
    
    return temperatura;
}

// Se a mensagem contiver um número inteiro positivo, utiliza o valor como intervalo de piscagem do LED
bool mensagem_inteiro_positivo(const char *texto, int *valor_saida) {
    if(texto == NULL || texto[0] == '\0') {
        return false;
    }

    int valor = 0;
    for(int i = 0; texto[i] != '\0'; i++) {
        // Verifica se o caractere atual não é um dígito numérico
        if(texto[i] < '0' || texto[i] > '9') {
            return false;
        }
        // Converte texto para número
        valor = valor * 10 + (texto[i] - '0');
    }

    if(valor <= 0) {
        return false;
    }
    *valor_saida = valor;
    
    return true;
}

bool conectar_com_servidor_mqtt() {
    ip_addr_t end_mqtt;
    ip4addr_aton(END_MQTT, &end_mqtt);

    mqtt_cliente = mqtt_client_new();

    mqtt_set_inpub_callback(
        mqtt_cliente,
        &mqtt_recebendo_publicacao_cb,
        &mqtt_dados_recebidos_cb,
        NULL
    );

    err_t houve_erro = mqtt_client_connect(
        mqtt_cliente,
        &end_mqtt,
        1883,
        &mqtt_conectado_cb,
        NULL,
        &info_cliente
    );

    if (houve_erro != ERR_OK){
        printf("Falha na requisição de conexão MQTT\n");
        return false;
    }
    return true;
}

bool configurar_wifi() {
    // Inicializa o chip Wi-Fi da placa
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar o chip Wi-Fi da placa\n");
        return false;
    }
    
    // Coloca o módulo Wi-Fi em modo estação (STA)
    cyw43_arch_enable_sta_mode();
    
    // Tenta conectar a placa com o Wifi
    if(cyw43_arch_wifi_connect_timeout_ms(
        "Senha",                 // Nome da rede
        "01234567",              // Senha da rede
        CYW43_AUTH_WPA2_AES_PSK, // Tipo de criptografia
        30000                    // Tempo máximo de espera para conexão (30 segundos)
    )) {
        sinalizar_erro_ao_tentar_conectar_com_wifi();
        return false;
    }
    sinalizar_wifi_conectado_com_sucesso();
    // Obtém o endereço IP atribuído à interface Wi-Fi
    uint8_t *endereco_ip = (uint8_t*) &(cyw43_state.netif[0].ip_addr.addr);
    printf("Endereço IP: %d.%d.%d.%d\n", endereco_ip[0], endereco_ip[1], endereco_ip[2], endereco_ip[3]);
    
    return true;
}

void sinalizar_erro_ao_tentar_conectar_com_wifi() {
    printf("Não foi possível conectar ao Wifi!");
    // 0 -> Desliga o led; 1 -> Liga o led
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(500);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

void sinalizar_wifi_conectado_com_sucesso() {
    printf("Wifi conectado com sucesso!");
    // 0 -> Desliga o led; 1 -> Liga o led
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(500);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(500);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(500);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

void inicializar_sensor_temperatura() {
    adc_init();                        // Inicializa o ADC
    adc_set_temp_sensor_enabled(true); // Liga o sensor de temperatura interno
    adc_select_input(4);               // Seleciona qual entrada ADC será lida
}
