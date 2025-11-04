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
#define DEBOUNCE_TIME 500


// Tilakone
enum state { LISTEN = 0, DETECTED_RIGHT, DETECTED_LEFT, WAIT_FOR_RESETTING };
volatile enum state programState = LISTEN;


// Globaalit muuttujat datan tallentamista varten 
volatile float g_accel_x = 0.0, g_accel_y = 0.0, g_accel_z = 1.0;
volatile float g_gyro_x = 0.0, g_gyro_y = 0.0, g_gyro_z = 0.0;


// Ongelma: nappia painaessa välilyöntejä tuli useampi, duck.ai hakukoneen esimerkistä mallia
// ottaen luotu yksinkertainen debouncaus käyttäen <time.h> kirjastoa. 
static void btn_fxn(uint gpio, uint32_t eventMask) {
    static uint32_t last_press_time = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());

    // Käsitellään vain ylösreuna ja suodatetaan bounce tällä yksinkertaisella debouncella
    if (gpio == SW2_PIN) {
        if ((current_time - last_press_time > DEBOUNCE_TIME)) {
            last_press_time = current_time;
            printf("\n");
            buzzer_play_tone(1000, 50);
        }
    }
    else if (gpio == SW1_PIN) {
        if ((current_time - last_press_time > DEBOUNCE_TIME)) {
            last_press_time = current_time;
            toggle_led();
            // Lisää tänne jotain omainisuuksia tulevaisuudessa
        }
    }
}


// AI: Claude Sonnet 4.5
// Prompt(?): Muuta funktio lukemaan dataa sensorilta ICM42670 ja päivittämään globaalit muuttujat jatkuvasti
// Muokattu funktioon oikeat kutsut mm. ICM42670_start_with_default_values. Muokattu aikoja sekä kommentteja.
static void sensor_task(void *arg){
    (void)arg;

    // Käynnistetään ICM42670-sensori oletusarvoilla
    if (ICM42670_start_with_default_values() != 0) {
        printf("IMU-sensorin käynnistys epäonnistui! Taski pysähtyy.\n");
        for(;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Muuttujat datan lukemista varten
    float ax, ay, az, gx, gy, gz, t;

    // Luetaan dataa ikuisessa loopissa
    for(;;){
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);

        // Tallennetaan uusin data globaaleihin muuttujiin
        g_accel_x = ax;
        g_accel_y = ay;
        g_accel_z = az;
        g_gyro_x = gx;
        g_gyro_y = gy;
        g_gyro_z = gz;

        // Kuinka monta kertaa sekunnissa dataa luetaan, atm kahdesti sekunnissa
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


// Funktio tutkii kiihtyvyysanturin dataa ja muokkaa tilakonetta sekä palauttaa käyttäjälle
// feedbackia LED:illä ja summerilla. Printtaa myös pisteet ja viivat serial monitoriin.
static void morse_task(void *arg){
    (void)arg;

    for(;;){
        float ax = g_accel_x;
        float az = g_accel_z;

        // Tarkista oikea käännös: x ~= 1 ja z ~= 0
        if (programState == LISTEN) {

            // Tarkista vasen käännös: x ~= -1 ja z ~= 0
            if (ax > -1.5 && ax < -0.5 && az < 0.5 && az > -0.5) {
                printf(".");
                toggle_led();
                buzzer_play_tone(4000, 100);
                programState = WAIT_FOR_RESETTING;

            // Tarkista oikea käännös: x ~= 1 ja z ~= 0
            } else if (ax < 1.5 && ax > 0.5 && az < 0.5 && az > -0.5) {
                printf("_");
                toggle_led();
                buzzer_play_tone(1000, 500);
                programState = WAIT_FOR_RESETTING;
            }

        } else if (programState == WAIT_FOR_RESETTING) {
            // Tarkista lepoasento: x ~= 0 ja z ~= 1
            if (ax < 0.5 && ax > -0.5 && az > 0.5) {
                programState = LISTEN;
                toggle_led();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/*
static void usbTask(void *arg) {
    (void)arg;
    while (1) {
        tud_task();              
    }
}*/


// AI: Claude Sonnet 4.5
// Prompt(?): Analysoi koodi main.c ja muokkaa main funktio toimivaksi. Älä luo uutta, muokkaa olemassa olevaa.
// Lisätty buzzerin alustus ja init_hat_sdk jonka AI poisti.
int main() {
    stdio_init_all();
    
    // Alustetaan tarvittavat asiat
    init_hat_sdk();
    sleep_ms(300); //Wait some time so initialization of USB and hat is done.

    init_buzzer();
    init_button1();
    init_button2();
    init_red_led();
    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);
    gpio_set_irq_enabled_with_callback(SW2_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);
    
    TaskHandle_t hSensorTask, hPrintTask, hUSB = NULL;

    // Luodaan taskit
    BaseType_t result = xTaskCreate(sensor_task, "sensor", DEFAULT_STACK_SIZE, NULL, 2, &hSensorTask);
    if(result != pdPASS) {
        printf("Sensor task creation failed\n");
        return 0;
    }
    result = xTaskCreate(morse_task, "morse", DEFAULT_STACK_SIZE, NULL, 2, &hPrintTask);
    if(result != pdPASS) {
        printf("Print Task creation failed\n");
        return 0;
    }

    // Käynnistetään FreeRTOS
    vTaskStartScheduler();
    return 0;
}