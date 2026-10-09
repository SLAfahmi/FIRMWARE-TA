#pragma once
#include <Arduino.h>

// Operasi Fuzzy Dasar
float fungsiMin(float a, float b);
float fungsiMin3(float a, float b, float c);
float fungsiMax(float a, float b);

// Fungsi Keanggotaan Kelembaban Tanah (%)
float f_Kering(float y);
float f_Lembab(float y);
float f_Basah(float y);

// Fungsi Keanggotaan Suhu (°C)
float f_Dingin(float x);
float f_Normal(float x);
float f_Panas(float x);

// Fungsi Keanggotaan Cahaya (Lux)
float f_Redup(float x);
float f_Sedang(float x);
float f_Terang(float x);

// Fungsi Keanggotaan Durasi Penyiraman Output (Detik)
float f_Mati(float z);
float f_Pendek(float z);
float f_SedangOut(float z);
float f_Lama(float z);

// Evaluasi Inferensi Mamdani & Defuzzifikasi Centroid
float prosesFuzzyMamdani(float kelembaban, float suhu, float cahaya);
