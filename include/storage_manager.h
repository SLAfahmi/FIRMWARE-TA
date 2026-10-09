#pragma once
#include <Arduino.h>
#include <LittleFS.h>

bool initStorage();
bool saveLogFile();
bool loadLogFile();
void saveSchedule();
void loadSchedule();
void saveToOfflineBuffer(const String &jsonPayload);
void flushOfflineBuffer();
