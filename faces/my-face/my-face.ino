#include "MyFaceWatchy.h"
#include "settings.h"

MyFaceWatchy watchy(settings);

void setup(){
  // a fresh boot (e.g. after uploading) wipes the clock, so ask the face to sync it over Wi-Fi
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED){
    watchy.requestTimeSync();
  }
  watchy.start();
}

void loop(){}
