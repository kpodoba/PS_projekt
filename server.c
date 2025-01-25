#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ifaddrs.h>

#define PORT 12345
#define MAX_CLIENTS 10
#define WORD_LENGTH 32
#define TLV_VALUE_SIZE 64
#define MAX_ATTEMPTS 6

struct TLV {
    uint8_t type;
    uint8_t length;
    char value[TLV_VALUE_SIZE];
};

struct GameState {
    char word[WORD_LENGTH];
    char guessed[WORD_LENGTH];
    int attempts_left;
};

const char *words[] = {"computer", "network", "socket", "thread", "process"};

void daemonize();
void *client_handler(void *arg);
void *multicast_announcer(void *arg);
char *select_word();
void init_game(struct GameState *game);
void process_guess(struct GameState *game, char guess, struct TLV *response);

void handle_signal(int sig) {
    syslog(LOG_INFO, "Serwer wyłączany sygnałem %d", sig);
    closelog();
    exit(0);
}

int main() {
    daemonize();

    openlog("server", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "Serwer uruchomiony w trybie demona");

    signal(SIGINT, handle_signal);

    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        syslog(LOG_ERR, "Błąd tworzenia gniazda: %m");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "Błąd wiązania: %m");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_CLIENTS) == -1) {
        syslog(LOG_ERR, "Błąd listen(): %m");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Serwer działa na porcie %d", PORT);

    // Uruchomienie wątku multicastowego
    pthread_t multicast_thread;
    if (pthread_create(&multicast_thread, NULL, multicast_announcer, NULL) != 0) {
        syslog(LOG_ERR, "Błąd tworzenia wątku multicast: %m");
        exit(EXIT_FAILURE);
    }

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            syslog(LOG_WARNING, "Błąd akceptowania połączenia: %m");
            continue;
        }

        syslog(LOG_INFO, "Nowy klient połączony: IP=%s, Port=%d, FD=%d",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, (void *)(intptr_t)client_fd) != 0) {
            syslog(LOG_ERR, "Błąd tworzenia wątku: %m");
            close(client_fd);
        }
    }

    close(server_fd);
    syslog(LOG_INFO, "Serwer wyłączony");
    closelog();
    return 0;
}

void daemonize() {
    pid_t pid = fork();

    if (pid < 0) {
        perror("Błąd forka");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("Błąd tworzenia sesji");
        exit(EXIT_FAILURE);
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();

    if (pid < 0) {
        perror("Błąd drugiego forka");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (chdir("/") < 0) {
        perror("Błąd zmiany katalogu roboczego");
        exit(EXIT_FAILURE);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
}

void *multicast_announcer(void *arg) {
    int sock;
    struct sockaddr_in multicast_addr;
    const char *multicast_ip = "239.255.255.250";
    const int multicast_port = 12346;
    char message[64];
    char server_ip[INET_ADDRSTRLEN] = {0};

    // Dynamiczne pobieranie adresu IP serwera
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        syslog(LOG_ERR, "Błąd pobierania adresów IP: %m");
        pthread_exit(NULL);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
            continue;

        // Ignoruj interfejsy loopback
        if (strcmp(ifa->ifa_name, "lo") == 0)
            continue;

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, server_ip, sizeof(server_ip));
        break; // Pobierz pierwszy znaleziony adres
    }
    freeifaddrs(ifaddr);

    if (strlen(server_ip) == 0) {
        syslog(LOG_ERR, "Nie znaleziono odpowiedniego adresu IP serwera");
        pthread_exit(NULL);
    }

    syslog(LOG_INFO, "Adres IP serwera: %s", server_ip);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        syslog(LOG_ERR, "Błąd tworzenia gniazda multicast: %m");
        pthread_exit(NULL);
    }

    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = inet_addr(multicast_ip);
    multicast_addr.sin_port = htons(multicast_port);

    snprintf(message, sizeof(message), "Server:%s:%d", server_ip, PORT);

    while (1) {
        if (sendto(sock, message, strlen(message), 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr)) < 0) {
            syslog(LOG_ERR, "Błąd wysyłania multicast: %m");
        }
        sleep(2); // Wysyłaj co 2 sekundy
    }

    close(sock);
    pthread_exit(NULL);
}

void *client_handler(void *arg) {
    int client_fd = (intptr_t)arg;
    struct GameState game;
    struct TLV request, response;

    init_game(&game);

    while (game.attempts_left > 0) {
        if (recv(client_fd, &request, sizeof(request), 0) <= 0) {
            syslog(LOG_INFO, "Klient FD=%d rozłączył się. Zamykam połączenie.", client_fd);
            close(client_fd);
            pthread_exit(NULL);
        }

        char guess = request.value[0];
        syslog(LOG_INFO, "Klient FD=%d zgaduje literę: %c", client_fd, guess);

        process_guess(&game, guess, &response);

        if (send(client_fd, &response, sizeof(response), 0) <= 0) {
            syslog(LOG_ERR, "Błąd wysyłania do klienta FD=%d: %m", client_fd);
            break;
        }

        if (response.type == 2) {
            if (game.attempts_left > 0 && strcmp(game.word, game.guessed) == 0) {
                syslog(LOG_INFO, "Klient FD=%d WYGRAŁ grę! Słowo: %s", client_fd, game.word);
            } else {
                syslog(LOG_INFO, "Klient FD=%d PRZEGRAŁ grę! Słowo: %s", client_fd, game.word);
            }
            break;
        }
    }

    close(client_fd);
    pthread_exit(NULL);
}

void init_game(struct GameState *game) {
    strcpy(game->word, select_word());
    memset(game->guessed, '_', strlen(game->word));
    game->guessed[strlen(game->word)] = '\0';
    game->attempts_left = MAX_ATTEMPTS;
}

char *select_word() {
    srand(time(NULL));
    return (char *)words[rand() % (sizeof(words) / sizeof(words[0]))];
}

void process_guess(struct GameState *game, char guess, struct TLV *response) {
    int found = 0;
    size_t word_length = strlen(game->word);

    for (size_t i = 0; i < word_length; i++) {
        if (game->word[i] == guess) {
            game->guessed[i] = guess;
            found = 1;
        }
    }

    if (!found) {
        game->attempts_left--;
    }

    if (strcmp(game->word, game->guessed) == 0) {
        response->type = 2;
        snprintf(response->value, sizeof(response->value), "Wygrana! Słowo: %s", game->word);
    } else if (game->attempts_left == 0) {
        response->type = 2;
        snprintf(response->value, sizeof(response->value), "Przegrana! Słowo: %s", game->word);
    } else {
        response->type = found ? 1 : 0;
        snprintf(response->value, sizeof(response->value), "Słowo: %s | Próby: %d", game->guessed, game->attempts_left);
    }

    response->length = strlen(response->value);
}
