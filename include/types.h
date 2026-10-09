#pragma once
#include <Arduino.h>
#include "config.h"

// Struktur status zona
struct Zona {
  float soil;
  float soilBefore;
  float duration;
  int mode; // 0=Off/Idle, 1=Short, 2=Medium, 3=Long/Schedule, 4=Queued
};

// Struktur jadwal penyiraman harian
struct Schedule {
  bool active;
  uint8_t hour;
  uint8_t minute;
  uint32_t duration;
};

// Struktur entri log historis
struct LogEntry {
  char timestamp[20];
  float temp;
  float hum;
  float light;
  float soilBefore[NUM_ZONES];
  float soilAfter[NUM_ZONES];
  int mode[NUM_ZONES];
  float duration[NUM_ZONES];
  int sysMode;
};
