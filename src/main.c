#include <stdio.h>
#include <string.h>
#include <math.h>

#include <pico/stdlib.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "tkjhat/sdk.h"

#define DEFAULT_STACK_SIZE 2048
#define CDC_ITF_TX      1

// Tilakone
enum state { REPOSE = 0, DETECTED_RIGHT, DETECTED_LEFT, WAIT_FOR_REPOSE };
volatile enum state programState = REPOSE;
// Globaalit muuttujat datan tallentamista varten 
volatile float g_accel_x = 0.0f, g_accel_y = 0.0f, g_accel_z = 1.0f;
volatile float g_gyro_x = 0.0f, g_gyro_y = 0.0f, g_gyro_z = 0.0f;


static void btn_fxn(uint gpio, uint32_t eventMask) {
    // Vaihtaa punaisen LEDin tilaa
    toggle_led();
}

static void sensor_task(void *arg){
    (void)arg;

    // Käynnistetään ICM42670-sensori oletusarvoilla
    if (ICM42670_start_with_default_values() != 0) {
        printf("IMU-sensorin käynnistys epäonnistui! Taski pysähtyy.\n");
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
   
    // Muuttujat datan lukemista varten
    float ax, ay, az, gx, gy, gz, t;

    for(;;){
        
        // Luetaan data sensorilta
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);

    // Tallennetaan uusin anturidata globaaleihin muuttujiin
    g_accel_x = ax;
    g_accel_y = ay;
    g_accel_z = az;
    g_gyro_x = gx;
    g_gyro_y = gy;
    g_gyro_z = gz;

    // En tiedä mikä tämä on, mutta ilmeisesti tarvitaan
    vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void morse_task(void *arg){
    (void)arg;

    // Threshold-arvot: odotetaan täsmällisiä arvoja (-1,0,1), mutta salli pieni toleranssi
    const float tol = 0.25f; // säädä tarvittaessa

    for(;;){
        float ax = g_accel_x;
        float az = g_accel_z;

        // Tarkista oikea käännös: x ~= 1 ja z ~= 0
        if (programState == REPOSE) {
            if (fabsf(ax - 1.0f) <= tol && fabsf(az - 0.0f) <= tol) {
                // Vasen -> '.'
                printf(".");
                fflush(stdout);
                buzzer_play_tone(1000, 100); // Soita 1000 Hz sävel 100 ms
                programState = WAIT_FOR_REPOSE; // odota lepoasentoon paluuta
            } else if (fabsf(ax - (-1.0f)) <= tol && fabsf(az - 0.0f) <= tol) {
                // Oikea -> '_'
                printf("_");
                fflush(stdout);
                buzzer_play_tone(1000, 500);
                programState = WAIT_FOR_REPOSE; // odota lepoasentoon paluuta
            }
        } else if (programState == WAIT_FOR_REPOSE) {
            // Odotetaan lepoasentoa: x ~= 0 ja z ~= 1
            if (fabsf(ax - 0.0f) <= tol && fabsf(az - 1.0f) <= tol) {
                // Palattu lepoasentoon -> palaa REPOSE-tilaan ja odota seuraavaa käännöstä
                programState = REPOSE;
                toggle_led();
                buzzer_play_tone(4000, 100);
                vTaskDelay(pdMS_TO_TICKS(100));
                toggle_led();
                // Lähetä lineaali, ei tulostusta nyt. Seuraava käännös tulostaa merkin.
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

int main() {
    stdio_init_all();
    
    // Alustetaan tarvittavat asiat
    init_hat_sdk();
    sleep_ms(300); //Wait some time so initialization of USB and hat is done.

    init_buzzer();
    init_button1();
    init_red_led();
    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_FALL, true, btn_fxn);

    
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