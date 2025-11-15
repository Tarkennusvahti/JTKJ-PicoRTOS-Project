#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pico/stdlib.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "tkjhat/sdk.h"

#define DEFAULT_STACK_SIZE 2048
#define CDC_ITF_TX      1
#define DEBOUNCE_TIME 250
#define INPUT_BUFFER_SIZE 256


// Funktioiden prototyypit
static void btn_fxn(uint gpio, uint32_t eventMask);
static void print_task(void *arg);
static void sensor_task(void *arg);
static void morse_task(void *arg);
static void receive_task(void *arg);
char decode_morse_letter(const char *morse);
void decode_morse_message(char *morse_input, char *output, size_t output_size);


// Morsekooditaulukko
static const char *morse_table[] = {
    ".-",      // A
    "-...",    // B
    "-.-.",    // C
    "-..",     // D
    ".",       // E
    "..-.",    // F
    "--.",     // G
    "....",    // H
    "..",      // I
    ".---",    // J
    "-.-",     // K
    ".-..",    // L
    "--",      // M
    "-.",      // N
    "---",     // O
    ".--.",    // P
    "--.-",    // Q
    ".-.",     // R
    "...",     // S
    "-",       // T
    "..-",     // U
    "...-",    // V
    ".--",     // W
    "-..-",    // X
    "-.--",    // Y
    "--..",    // Z
    NULL       // Lopetus
};


// Tilakone morsetukselle
enum state { LISTEN = 0, DETECTED_RIGHT, DETECTED_LEFT, WAIT_FOR_RESETTING };
volatile enum state programState = LISTEN;

// Tilakone printtaukselle
enum printState { LISTEN_PRINT = 0, BUTTON1_PRESSED, BUTTON2_PRESSED };
volatile enum printState printState = LISTEN_PRINT;


// Globaalit muuttujat datan tallentamista varten
volatile float pos_x = 0.0, pos_y = 0.0, pos_z = 1.0;
volatile float accel_x = 0.0, accel_y = 0.0, accel_z = 0.0;
char translate[INPUT_BUFFER_SIZE];
char temp_morse[INPUT_BUFFER_SIZE];


// Ongelma: nappia painaessa välilyöntejä tuli useampi, duck.ai hakukoneen esimerkistä mallia
// ottaen luotu yksinkertainen debouncaus käyttäen <time.h> kirjastoa. 
static void btn_fxn(uint gpio, uint32_t eventMask) {
    static uint32_t last_press_time = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());

    // Käsitellään vain ylösreuna ja suodatetaan bounce tällä yksinkertaisella debouncella
    if (gpio == SW2_PIN && (eventMask & GPIO_IRQ_EDGE_RISE)) {
        if ((current_time - last_press_time > DEBOUNCE_TIME)) {
            last_press_time = current_time;
            printState = BUTTON2_PRESSED;
        }
    }
    else if (gpio == SW1_PIN && (eventMask & GPIO_IRQ_EDGE_RISE)) {
        if ((current_time - last_press_time > DEBOUNCE_TIME)) {
            last_press_time = current_time;
            printState = BUTTON1_PRESSED;
        }
    }
}


// AI: Claude Sonnet 4.5
// Prompt: Muuta funktio lukemaan dataa sensorilta ICM42670 ja päivittämään globaalit muuttujat jatkuvasti
// Muokattu funktioon oikeat kutsut mm. ICM42670_start_with_default_values. Muokattu aikoja sekä kommentteja.
static void sensor_task(void *arg){
    (void)arg;

    // Muuttujat datan lukemista varten
    float px, py, pz, ax, ay, az, t;

    // Luetaan dataa ikuisessa loopissa
    for(;;){
        ICM42670_read_sensor_data(&px, &py, &pz, &ax, &ay, &az, &t);

        // Tallennetaan uusin data globaaleihin muuttujiin
        // Poistetu turhat muuttujat, käytetään vain x ja z akselia
        pos_x = px;
        pos_z = pz;

        // Kuinka monta kertaa sekunnissa dataa luetaan, atm kahdesti sekunnissa
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


// Funktio tutkii kiihtyvyysanturin dataa ja muokkaa tilakonetta sekä palauttaa käyttäjälle
// feedbackia LED:illä ja summerilla. Printtaa myös pisteet ja viivat serial monitoriin.
static void morse_task(void *arg){
    (void)arg;

    for(;;){
        float px = pos_x;
        float pz = pos_z;

        // Tarkista oikea käännös: x ~= 1 ja z ~= 0
        if (programState == LISTEN) {

            // Tarkista vasen käännös: x ~= -1 ja z ~= 0
            if (px > -1.5 && px < -0.5 && pz < 0.5 && pz > -0.5) {
                printf(".");
                toggle_led();
                buzzer_play_tone(4000, 100);

                // Lisätään globaaliinmerkkijonoon piste, käännetään myöhemmin
                strcat(translate, ".");
                strcat(temp_morse, ".");
                write_text(temp_morse);

                programState = WAIT_FOR_RESETTING;

            // Tarkista oikea käännös: x ~= 1 ja z ~= 0
            } else if (px < 1.5 && px > 0.5 && pz < 0.5 && pz > -0.5) {
                printf("-");
                toggle_led();
                buzzer_play_tone(1000, 500);

                // Lisätään globaali merkkijonoon viiva, käännetään myöhemmin
                strcat(translate, "-");
                strcat(temp_morse, "-");
                write_text(temp_morse);

                programState = WAIT_FOR_RESETTING;
            }

        } else if (programState == WAIT_FOR_RESETTING) {
            // Tarkista lepoasento: x ~= 0 ja z ~= 1
            if (px < 0.5 && px > -0.5 && pz > 0.5) {
                programState = LISTEN;
                toggle_led();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


// AI: Claude Sonnet 4.5
// Prompt: Luo funktio joka kääntää konsolesta annetun morse-koodin takaisin kirjaimiksi
// Lisätty kommentteja selventämään toimintaa
// Funktio on apufunktio decode_morse_message funktiolle, decoodaa yhden yksittäisen kirjaimen
char decode_morse_letter(const char *morse) {
    // Tarkista kirjaimet A-Z ja vertaa niitä morse-koodiin
    for (int i = 0; i < 26; i++) {
        if (strcmp(morse, morse_table[i]) == 0) {
            // Jos löydetään pari, palautetaan vastaava kirjain ('A' = 65)
            // Esim B kirjaimella, i=1, palautetaan 'A' + 1 = 'B'
            return 'A' + i;
        }
    }
    return '?';  // Tuntematon morsekoodi
}


// Funktio luotu ylläolevan promptin yhteydessä
// 
// Muokkaa suoraan syötettä (ei kopioi)
void decode_morse_message(char *morse_input, char *output, size_t output_size) {
    size_t out_idx = 0;
    char *token = strtok(morse_input, " ");  // Muokkaa SUORAAN morse_input:ia
    
    while (token != NULL && out_idx < output_size - 1) {
        char decoded = decode_morse_letter(token);
        output[out_idx++] = decoded;
        token = strtok(NULL, " ");
    }
    
    output[out_idx] = '\0';
}


// Taski tarkistaa tilakoneen avulla nappien tilaa ja toteuttaa sen mukaiset toimenpiteet.
// Korjasi kriittisen ongelman, jossa ohjelma kaatui kun kutsu tuli isr sisällä
static void print_task(void *arg){
    (void)arg;
    char decoded_message[INPUT_BUFFER_SIZE];

    for(;;){
        if (printState == BUTTON1_PRESSED) {
            buzzer_play_tone(1000, 50);
            clear_display();

            if (translate[0] != '\0') {
                decode_morse_message(translate, decoded_message, INPUT_BUFFER_SIZE);
                printf("\nDecoded message: %s\n", decoded_message);
                write_text(decoded_message);
            } else {
                printf("Resetting, clearing display.\n");
            }

            // Tyhjennetään globaali merkkijono
            translate[0] = '\0';

            printState = LISTEN_PRINT;
        } else if (printState == BUTTON2_PRESSED) {
            printf(" ");
            buzzer_play_tone(1000, 50);

            // Lisätään globaaliin merkkijonoon välilyönti kirjainten erottamiseksi
            strcat(translate, " ");
            temp_morse[0] = '\0'; // Tyhjennetään väliaikainen morse-merkkijono
            clear_display();

            printState = LISTEN_PRINT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


static void receive_task(void *arg){
    (void)arg;
    char line[INPUT_BUFFER_SIZE];
    size_t index = 0;
    
    for(;;){
        //OPTION 1
        // Using getchar_timeout_us https://www.raspberrypi.com/documentation/pico-sdk/runtime.html#group_pico_stdio_1ga5d24f1a711eba3e0084b6310f6478c1a
        // take one char per time and store it in line array, until reeceived the \n
        // The application should instead play a sound, or blink a LED. 
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT){// I have received a character
            if (c == '\r') continue; // ignore CR, wait for LF if (ch == '\n') { line[len] = '\0';
            if (c == '\n'){
                // terminate and process the collected line
                line[index] = '\0'; 
                // DEKOODAA morse-viesti
                char decoded_message[INPUT_BUFFER_SIZE];
                decode_morse_message(line, decoded_message, INPUT_BUFFER_SIZE);
                
                // Tulosta debug
                printf("Morse: \"%s\" → \"%s\"\n", line, decoded_message);
                clear_display();
                write_text(decoded_message); // Show the morse code on the display

                buzzer_play_tone(2000, 50); // Indicate message received
                vTaskDelay(pdMS_TO_TICKS(50));
                buzzer_play_tone(2000, 50);

                index = 0;
                vTaskDelay(pdMS_TO_TICKS(100)); // Wait for new message
            }
            else if(index < INPUT_BUFFER_SIZE - 1){
                line[index++] = (char)c;
            }
            else { //Overflow: print and restart the buffer with the new character. 
                line[INPUT_BUFFER_SIZE - 1] = '\0';
                printf("Morsed: %s\n", line);
                index = 0; 
                line[index++] = (char)c; 
            }
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait for new message
        }
    }
}


// AI: Claude Sonnet 4.5
// Prompt: Analysoi koodi main.c ja muokkaa main funktio toimivaksi. Älä luo uutta, muokkaa olemassa olevaa.
// Lisätty buzzerin alustus ja init_hat_sdk jonka AI poisti.
int main() {
    stdio_init_all();
    
    // Alustetaan tarvittavat asiat
    stdio_init_all();   // Tuki USB:lle
    init_hat_sdk();     // Ledi pois ja i2c alustukset
    sleep_ms(300);      //Wait some time so initialization of USB and hat is done.

    init_ICM42670(); // Alustetaan ICM42670 sensori
    sleep_ms(300);      // Odotetaan hetki
    ICM42670_start_with_default_values();

    init_buzzer();
    init_button1();
    init_button2();
    init_red_led();
    init_display();
    clear_display();

    // Asetetaan keskeytyksen käsittelijät
    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);
    gpio_set_irq_enabled_with_callback(SW2_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);

    TaskHandle_t hSensorTask, hMorseTask, hPrintTask, hReceiveTask = NULL;

    // Luodaan taskit pyörimään taustalle ja tarkistetaan onnistuiko
    BaseType_t result = xTaskCreate(sensor_task, "sensor", DEFAULT_STACK_SIZE, NULL, 2, &hSensorTask);
    if(result != pdPASS) {
        printf("Sensor task creation failed\n");
        return 0;
    }

    result = xTaskCreate(morse_task, "morse", DEFAULT_STACK_SIZE, NULL, 2, &hMorseTask);
    if(result != pdPASS) {
        printf("Morse Task creation failed\n");
        return 0;
    }

    result = xTaskCreate(print_task, "print", DEFAULT_STACK_SIZE, NULL, 2, &hPrintTask);
    if(result != pdPASS) {
        printf("Print Task (CMD) creation failed\n");
        return 0;
    }

    result = xTaskCreate(receive_task, "receive", DEFAULT_STACK_SIZE, NULL, 2, &hReceiveTask);
    if(result != pdPASS) {
        printf("Receive Task creation failed\n");
        return 0;
    }

    // Käynnistetään FreeRTOS
    vTaskStartScheduler();
    return 0;
}