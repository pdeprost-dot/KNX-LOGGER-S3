# Plan de validation matérielle

1. 10 min à 83 333 S/s, Web actif : exiger `lost=0`, `dma_overflow=0`, `buffer_overflow=0`, `sd_errors=0`.
2. Refaire 10 min à 100 000, 125 000, 166 667 puis 200 000 S/s. Arrêter à la première fréquence non parfaitement stable.
3. Une heure à la fréquence fiable retenue, avec consultations Web régulières.
4. Plusieurs heures sur power bank, puis validation du fichier par `python tools/inspect_raw.py fichier.knxraw`.

Une fréquence n'est déclarée fiable qu'après validation réelle. Un fichier sans enregistrement `END ` indique un arrêt non propre, mais les blocs `DATA` complets restent récupérables et vérifiables par CRC.


## Mesures du 23 septembre 2026

- 30,5 s : 2 529 280 samples acquis, zéro perte/overflow/erreur après correction de pile DMA.
- 121,3 s : 10 092 544 samples acquis et écrits, zéro perte, zéro overflow, zéro erreur ; 83 208 S/s observés ; environ 169,7 kB/s.
- Les essais longs ont ensuite révélé des erreurs physiques d'écriture SD à des positions variables (environ 5,9 à 77,8 MB), indépendantes de la limite FAT32 et reproduites à 4, 10 et 20 MHz. Le jalon 10 minutes n'est donc **pas validé** avec cette carte/liaison SD.
- Le firmware final arrête désormais l'acquisition dès la première écriture courte/échouée, allume ERROR et conserve l'absence de `session-end.json` comme marqueur d'interruption.

Avant de reprendre le jalon : vérifier la carte sur PC (test intégral lecture/écriture, formatage FAT32 complet), puis réinsérer et recommencer à 83 333 S/s. Ne pas augmenter la fréquence ADC.
### Validation après formatage FAT32 complet

Session `S-5B4CEF5D384AB1D3` : 603,9 s à 83 333 S/s demandés, 83 302,8 S/s observés, 50 304 000 samples acquis au dernier relevé avant STOP, zéro sample perdu, zéro overflow DMA, zéro overflow buffer, zéro erreur SD. `STOP` a été exécuté proprement. Taille avant fermeture : 102 571 424 octets ; débit moyen : 169 857 octets/s ; heap minimum : 171 916 octets ; PSRAM libre : 7 268 292 octets. Validation CRC et continuité sur PC encore requise après retrait de la carte.