#include "fuzzy_logic.h"

// ==========================================
// OPERASI FUZZY DASAR
// ==========================================
float fungsiMin(float a, float b) { return (a < b) ? a : b; }

float fungsiMin3(float a, float b, float c) {
  return fungsiMin(a, fungsiMin(b, c));
}

float fungsiMax(float a, float b) { return (a > b) ? a : b; }

// ==========================================
// INPUT 1: KELEMBABAN TANAH (%)
// ==========================================
// 1. Himpunan Kering
float f_Kering(float y) {
  if (y <= 30.0f)
    return 1.0f;
  if (y > 30.0f && y < 45.0f)
    return (45.0f - y) / (45.0f - 30.0f);
  return 0.0f;
}

// 2. Himpunan Lembab
float f_Lembab(float y) {
  if (y <= 30.0f || y >= 65.0f)
    return 0.0f;
  if (y > 30.0f && y < 45.0f)
    return (y - 30.0f) / (45.0f - 30.0f);
  if (y >= 45.0f && y <= 50.0f)
    return 1.0f;
  if (y > 50.0f && y < 65.0f)
    return (65.0f - y) / (65.0f - 50.0f);
  return 0.0f;
}

// 3. Himpunan Basah (65 ke atas = 1.0)
float f_Basah(float y) {
  if (y <= 50.0f)
    return 0.0f;
  if (y > 50.0f && y < 65.0f)
    return (y - 50.0f) / (65.0f - 50.0f);
  return 1.0f;
}

// ==========================================
// INPUT 2: SUHU UDARA (°C)
// ==========================================
float f_Dingin(float x) {
  if (x <= 22.0f)
    return 1.0f;
  if (x > 22.0f && x < 26.0f)
    return (26.0f - x) / (26.0f - 22.0f);
  return 0.0f;
}

float f_Normal(float x) {
  if (x <= 24.0f || x >= 32.0f)
    return 0.0f;
  if (x > 24.0f && x <= 28.0f)
    return (x - 24.0f) / (28.0f - 24.0f);
  if (x > 28.0f && x < 32.0f)
    return (32.0f - x) / (32.0f - 28.0f);
  return 0.0f;
}

float f_Panas(float x) {
  if (x <= 30.0f)
    return 0.0f;
  if (x > 30.0f && x < 35.0f)
    return (x - 30.0f) / (35.0f - 30.0f);
  return 1.0f;
}

// ==========================================
// INPUT 3: CAHAYA (Lux) -> Max 65.000 Lux
// ==========================================
float f_Redup(float x) {
  if (x <= 5000.0f)
    return 1.0f;
  if (x > 5000.0f && x < 25000.0f)
    return (25000.0f - x) / (25000.0f - 5000.0f);
  return 0.0f;
}

float f_Sedang(float x) {
  if (x <= 5000.0f || x >= 65000.0f)
    return 0.0f;
  if (x > 5000.0f && x <= 35000.0f)
    return (x - 5000.0f) / (35000.0f - 5000.0f);
  if (x > 35000.0f && x < 65000.0f)
    return (65000.0f - x) / (65000.0f - 35000.0f);
  return 0.0f;
}

float f_Terang(float x) {
  if (x <= 45000.0f)
    return 0.0f;
  if (x > 45000.0f && x < 65000.0f)
    return (x - 45000.0f) / (65000.0f - 45000.0f);
  return 1.0f;
}

// ==========================================
// OUTPUT: DURASI PENYIRAMAN (Detik)
// ==========================================
float f_Mati(float z) {
  if (z <= 0.0f)
    return 1.0f;
  if (z > 0.0f && z < 10.0f)
    return (10.0f - z) / (10.0f - 0.0f);
  return 0.0f;
}

float f_Pendek(float z) {
  if (z <= 10.0f || z >= 90.0f)
    return 0.0f;
  if (z > 10.0f && z <= 50.0f)
    return (z - 10.0f) / (50.0f - 10.0f);
  if (z > 50.0f && z < 90.0f)
    return (90.0f - z) / (90.0f - 50.0f);
  return 0.0f;
}

float f_SedangOut(float z) {
  if (z <= 70.0f || z >= 190.0f)
    return 0.0f;
  if (z > 70.0f && z <= 130.0f)
    return (z - 70.0f) / (130.0f - 70.0f);
  if (z > 130.0f && z < 190.0f)
    return (190.0f - z) / (190.0f - 130.0f);
  return 0.0f;
}

float f_Lama(float z) {
  if (z <= 170.0f)
    return 0.0f;
  if (z > 170.0f && z < 222.0f)
    return (z - 170.0f) / (222.0f - 170.0f);
  if (z >= 222.0f)
    return 1.0f;
  return 0.0f;
}

// ==========================================
// FUZZY MAMDANI PROSES & CENTROID DEFUZZIFIKASI
// ==========================================
float prosesFuzzyMamdani(float kelembaban, float suhu, float cahaya) {
  float mu_Kering = f_Kering(kelembaban);
  float mu_Lembab = f_Lembab(kelembaban);
  float mu_Basah = f_Basah(kelembaban);

  float mu_Dingin = f_Dingin(suhu);
  float mu_Normal = f_Normal(suhu);
  float mu_Panas = f_Panas(suhu);

  float mu_Redup = f_Redup(cahaya);
  float mu_Sedang = f_Sedang(cahaya);
  float mu_Terang = f_Terang(cahaya);

  float r_Mati = 0.0f;
  float r_Pendek = 0.0f;
  float r_Sedang = 0.0f;
  float r_Lama = 0.0f;

  // --- Rule Base ---
  // OUTPUT: SEDANG
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Normal, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Normal, mu_Terang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Terang));

  // OUTPUT: LAMA
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Dingin, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Redup));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Terang));

  // OUTPUT: PENDEK
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Sedang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Terang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Sedang));

  // OUTPUT: MATI
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Terang));

  // --- Defuzzifikasi (Metode Centroid) ---
  float pembilang = 0.0f;
  float penyebut = 0.0f;

  for (float z = 0.0f; z <= 222.0f; z += 1.0f) {
    float m = fungsiMin(f_Mati(z), r_Mati);
    float p = fungsiMin(f_Pendek(z), r_Pendek);
    float s = fungsiMin(f_SedangOut(z), r_Sedang);
    float l = fungsiMin(f_Lama(z), r_Lama);

    float total = fungsiMax(fungsiMax(m, p), fungsiMax(s, l));

    pembilang += z * total;
    penyebut += total;
  }

  if (penyebut == 0.0f)
    return 0.0f;

  float hasil = pembilang / penyebut;

  // safety guard: jika hasil sangat kecil mendekati nol, bulatkan ke 0.
  if (hasil < 10.0f)
    return 0.0f;

  return hasil;
}
