# ARP scanner

Le programme découvre les hôtes IPv4 du réseau local avec des requêtes ARP.

1. `network_select` liste les interfaces IPv4 actives et récupère IP, masque,
   MAC et index de l'interface choisie.
2. `main` ouvre un socket `AF_PACKET`, le limite au protocole ARP, puis lance
   le scan.
3. `arp_scan` envoie une requête à chaque adresse hôte, attend les réponses
   pendant 2 secondes après le dernier paquet, filtre les trames ARP invalides
   et affiche chaque IP/MAC une seule fois.
4. `ssh_console` garde ensuite la main dans le terminal. Elle demande le user
   et le mot de passe SSH une seule fois, puis `update` demande le mot de passe
   sudo au premier besoin. Les valeurs sont enregistrées dans
   `~/.config/nsimaj/config` avec les permissions `0600`.
   `update` détecte `/etc/os-release` sur chaque hôte et utilise
   `apt-get` pour Debian/Ubuntu ou `pacman` pour Arch. Une distribution
   inconnue est signalée sans exécuter de commande.
   Les connexions sont séquentielles : la sortie standard et les erreurs de
   chaque machine restent visibles, sans `screen`.
   Les erreurs sont aussi enregistrées dans `~/.config/nsimaj/ssh.log`.

La connexion utilise `sshpass` et l'authentification par mot de passe.
Le démon SSH doit déjà être actif sur les machines distantes. Les nouvelles
clés d'hôte sont acceptées automatiquement ; une clé déjà connue qui change
reste refusée et sera indiquée dans les logs.

Le programme demande automatiquement le mot de passe sudo au lancement,
valide les privilèges puis se relance en root. Il peut donc être lancé
directement :

```sh
./app
```

Compilation :

```sh
make warnings
```

`sshpass` doit être installé avant l'utilisation :

```sh
sudo apt install sshpass
```
