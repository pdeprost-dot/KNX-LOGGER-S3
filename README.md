# KNX LOGGER S3

Sonde expérimentale / boîte noire KNX TP1 sur **Seeed Studio XIAO ESP32-S3 Sense**. Le firmware conserve le signal ADC brut sur microSD ; l'analyse lourde reste sur PC dans KNX TP1 Analyzer Studio.

> Avertissement : ne jamais connecter le bus KNX directement à l'ADC. Le diviseur 390 kΩ / 27 kΩ décrit pour les essais est non isolé, non protégé et n'est pas un AFE de production.

## État du jalon

Firmware initial compilable : ADC1 continu DMA à 83 333 S/s, buffers PSRAM, writer SD indépendant, format KXLR/1 avec CRC/gaps, START/STOP série-bouton-Web, point d'accès autonome et synchronisation navigateur. Les performances doivent être validées sur le matériel avant toute affirmation de zéro perte.

## Brochage vérifié

| Fonction | XIAO / GPIO | Note |
|---|---:|---|
| ADC KNX | D0 / GPIO1 / ADC1_CH0 | libre, compatible Wi-Fi |
| microSD | GPIO7 SCK, 8 MISO, 9 MOSI, 21 CS | carte Sense |
| bouton | D1 / GPIO2 vers GND | pull-up interne |
| BUS / REC / ERROR | D2/GPIO3, D3/GPIO4, D4/GPIO5 | LEDs externes avec résistances |
| Réserve SYNC | D5 / GPIO6 | non utilisée, ne pas connecter |

La LED utilisateur GPIO21 n'est pas utilisée car elle partage la sélection microSD. Sources : [pinout officiel Seeed](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) et [documentation microSD Sense](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/).

## Compiler et flasher

Prérequis : Arduino CLI, core `esp32:esp32` 3.3.11, sans PlatformIO.

```powershell
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi firmware
arduino-cli upload -p COM12 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi firmware
arduino-cli monitor -p COM12 -c baudrate=115200
```

Au démarrage, rejoindre le Wi-Fi `KNX-LOGGER-xxxx`, mot de passe `knxlogger`, puis ouvrir `http://192.168.4.1`. Pour le mode STA, copier `firmware/secrets.example.h` vers `firmware/secrets.h` ; ce dernier est ignoré par Git.

Sur le port série : `START`, `STOP`, `STATUS`.

## Architecture

`ADC continu/DMA (cœur 0, haute priorité) → pool PSRAM de 1 MiB (512 blocs) → queue → writer SD (cœur 1)`. Le serveur Web et les LEDs ne touchent jamais au chemin critique. Les pertes, overflows DMA/buffer et erreurs SD sont comptés et enregistrés.

Voir [format RAW](docs/RAW-FORMAT.md) et [plan de test](docs/TEST-PLAN.md). Les RAW, secrets Wi-Fi et données privées sont exclus du dépôt.


## Organisation de la microSD

Chaque START crée un dossier technique unique, jamais réutilisé :

```
/KNXLOGGER/device.json
/KNXLOGGER/S-<id-unique>/session.json
/KNXLOGGER/S-<id-unique>/events.jsonl
/KNXLOGGER/S-<id-unique>/raw-0001.bin
/KNXLOGGER/S-<id-unique>/session-end.json
```

Les segments RAW sont limités à 1 GiB, bien sous la limite FAT32 de 4 GiB. L'index global 64 bits et la séquence des records restent continus. L'absence de `session-end.json` identifie une session interrompue. Les champs humains de `session.json` sont volontairement vides au prototype et ne servent jamais d'identifiant technique.