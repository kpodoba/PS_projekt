#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 12345
#define WORD_LENGTH 32
#define TLV_VALUE_SIZE 64

// Struktura TLV do komunikacji między klientem a serwerem
struct TLV {
    uint8_t type;
    uint8_t length;
    char value[TLV_VALUE_SIZE];
};

// Deklaracje funkcji pomocniczych
void discover_server(char *server_ip, int *server_port);
void play_game(int server_fd);

int main() {
    char server_ip[INET_ADDRSTRLEN];
    int server_port;

    // Odszukanie serwera za pomocą multicast
    discover_server(server_ip, &server_port);

    printf("Znaleziono serwer: %s:%d\n", server_ip, server_port);

    int server_fd;
    struct sockaddr_in server_addr;

    // Tworzenie gniazda sieciowego
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Błąd tworzenia gniazda");
        exit(EXIT_FAILURE);
    }

    // Konfiguracja adresu serwera
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Nieprawidłowy adres serwera");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Nawiązanie połączenia z serwerem
    if (connect(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Błąd połączenia");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Połączono z serwerem.\n");

    // Rozpoczęcie gry
    play_game(server_fd);

    // Zamknięcie połączenia
    close(server_fd);
    return 0;
}

// Funkcja wyszukująca serwer w sieci lokalnej przez multicast
void discover_server(char *server_ip, int *server_port) {
    int sock;
    struct sockaddr_in multicast_addr;
    char buffer[64];
    socklen_t addr_len = sizeof(multicast_addr);
    const char *multicast_ip = "239.255.255.250";
    const int multicast_port = 12346;

    // Tworzenie gniazda UDP do odbioru multicast
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Błąd tworzenia gniazda multicast");
        exit(EXIT_FAILURE);
    }

    // Konfiguracja adresu multicast
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    multicast_addr.sin_port = htons(multicast_port);

    // Przypisanie gniazda do portu multicast
    if (bind(sock, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr)) < 0) {
        perror("Błąd bindowania gniazda");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Dołączenie do grupy multicast
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("Błąd dołączania do grupy multicast");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Oczekiwanie na wiadomość multicast od serwera
    printf("Oczekiwanie na ogłoszenie serwera...\n");
    if (recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&multicast_addr, &addr_len) < 0) {
        perror("Błąd odbierania multicast");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Parsowanie otrzymanej wiadomości
    if (sscanf(buffer, "Server:%[^:]:%d", server_ip, server_port) != 2) {
        fprintf(stderr, "Nieprawidłowy format komunikatu multicast: %s\n", buffer);
        close(sock);
        exit(EXIT_FAILURE);
    }

    close(sock);
}

// Funkcja obsługująca rozgrywkę
void play_game(int server_fd) {
    struct TLV request, response;
    char guess;

    while (1) {
        printf("Podaj literę: ");
        scanf(" %c", &guess);

        // Wysłanie zgadywanej litery do serwera w formacie TLV
        request.type = 1;
        request.length = 1;
        request.value[0] = guess;
        if (send(server_fd, &request, sizeof(request), 0) <= 0) {
            perror("Błąd wysyłania");
            break;
        }

        // Otrzymanie odpowiedzi od serwera
        if (recv(server_fd, &response, sizeof(response), 0) <= 0) {
            perror("Błąd odbierania - serwer prawdopodobnie zakończył połączenie");
            break;
        }

        // Sprawdzenie, czy gra się zakończyła
        if (response.type == 2) { // Koniec gry
            printf("%s\n", response.value);
            break;
        } else { // Odpowiedź po zgadywaniu
            printf("%s\n", response.value);
        }
    }

    printf("Połączenie z serwerem zostało zakończone.\n");
}
