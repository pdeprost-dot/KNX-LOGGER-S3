# Format KXLR/1

Tous les entiers sont little-endian. Un fichier `.knxraw` commence par un en-tête fixe de 512 octets, puis une suite d'enregistrements. Les échantillons `DATA` sont les codes ADC 12 bits non calibrés, stockés tels quels dans des `uint16_t`. Aucun filtre n'est appliqué.

## En-tête

La structure exacte est `FileHeader` dans `firmware/raw_format.h`. Elle contient la magie `KNXRAW1\0`, la version, les identifiants recorder/session, la fréquence demandée, la configuration ADC, l'association éventuelle epoch navigateur/temps monotone et un CRC-32 IEEE sur les 508 premiers octets.

## Enregistrements

Chaque enregistrement commence par `RecordHeader` (40 octets) : FourCC du type, version, tailles, séquence, index absolu du premier sample, temps monotone en µs et CRC-32 du payload.

- `DATA` : tableau contigu de `uint16_t` ADC.
- `GAP ` : nombre de samples perdus et cause. Une perte n'est jamais dissimulée.
- `TIME` : association epoch ms / index sample / compteur monotone.
- `STAT` : compteurs finaux, fréquence observée, mémoires minimales et taille.
- `END ` : fermeture propre.

L'index sample est la référence temporelle. Le temps idéal relatif vaut `sample_index / requested_sample_rate_hz`. Les points `TIME`, futurs événements communs KNX et formes d'onde permettent de corriger offset et dérive entre loggers.

Le lecteur indépendant `tools/inspect_raw.py` vérifie les CRC et résume un fichier sans dépendre du firmware.


## Arborescence et cycle de vie

La racine est `/KNXLOGGER`. `device.json` décrit le recorder stable. Chaque START crée exclusivement un nouveau `S-<id-unique>` après vérification d'absence ; aucun fichier existant n'est ouvert en écriture.

`session.json` est créé avant le démarrage ADC et contient les métadonnées humaines séparées de l'identité technique, les références temporelles, la configuration ADC/AFE, le matériel, le firmware, le format et la politique FAT32. `events.jsonl` journalise START, STOP, TIME_SYNC, ouvertures/fermetures de segments, gaps, overflows et erreurs avec compteur sample, temps monotone et epoch civil éventuel. `session-end.json` n'existe qu'après STOP propre et porte le bilan final `CLOSED`.

Les fichiers `raw-NNNN.bin` sont plafonnés à 1 GiB. Chaque segment possède son propre `FileHeader`, mais les `RecordHeader.first_sample_index` et `sequence` restent globaux à la session. Leur tri numérique reconstruit donc une timeline sans ambiguïté. Un changement de segment est effectué par le writer tandis que le pool PSRAM continue d'absorber les samples.