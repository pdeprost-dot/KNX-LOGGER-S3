# Plan de validation matérielle

1. 10 min à 83 333 S/s, Web actif : exiger `lost=0`, `dma_overflow=0`, `buffer_overflow=0`, `sd_errors=0`.
2. Refaire 10 min à 100 000, 125 000, 166 667 puis 200 000 S/s. Arrêter à la première fréquence non parfaitement stable.
3. Une heure à la fréquence fiable retenue, avec consultations Web régulières.
4. Plusieurs heures sur power bank, puis validation du fichier par `python tools/inspect_raw.py fichier.knxraw`.

Une fréquence n'est déclarée fiable qu'après validation réelle. Un fichier sans enregistrement `END ` indique un arrêt non propre, mais les blocs `DATA` complets restent récupérables et vérifiables par CRC.

