#include "net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

/* Estados internos da máquina de estado do processo de rede */
typedef enum { NET_IDLE, NET_LISTENING, NET_CONNECTED } NetState;


/*
    Lê uma linha do pipe de comandos sem bloquear.
    Retorna 1 se leu algo, 0 se o pipe está vazio, -1 se foi fechado.
*/
static int read_cmd(HANDLE h_read, char *buf, int buf_sz) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h_read, NULL, 0, NULL, &avail, NULL)) return -1;
    if (avail == 0) return 0;

    DWORD bytes_read = 0;
    if (!ReadFile(h_read, buf, buf_sz - 1, &bytes_read, NULL) || bytes_read == 0)
        return -1;

    buf[bytes_read] = '\0';
    /* Remove '\n' final se houver */
    int len = (int)bytes_read;
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return 1;
}


/*
    Envia um evento de volta para a UI pelo pipe de escrita.
*/
static void send_evt(HANDLE h_write, const char *evt) {
    DWORD written;
    WriteFile(h_write, evt, (DWORD)strlen(evt), &written, NULL);
}


/*
    Configura um socket como não-bloqueante usando ioctlsocket.
    Equivale ao fcntl(fd, F_SETFL, O_NONBLOCK) do Linux.
*/
static void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}


void run_net(HANDLE h_read, HANDLE h_write) {
    /* Inicializa Winsock — obrigatório antes de qualquer chamada de socket */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        send_evt(h_write, EVT_ERROR "WSAStartup falhou");
        return;
    }

    NetState state   = NET_IDLE;
    SOCKET   srv     = INVALID_SOCKET; /* socket servidor (modo LISTEN) */
    SOCKET   peer    = INVALID_SOCKET; /* socket da conexão ativa */

    char cmd[512];
    char evt[600];
    char recv_buf[512];

    while (1) {
        Sleep(50); /* throttle: 20 verificações/segundo sem busy-wait */

        /* Lê comando vindo da UI */
        int r = read_cmd(h_read, cmd, sizeof(cmd));
        if (r == -1) break; /* pipe fechado → encerra */

        if (r == 1) {
            /* CMD: LISTEN — abre servidor TCP na porta NET_PORT */
            if (strcmp(cmd, CMD_LISTEN) == 0 && state == NET_IDLE) {
                srv = socket(AF_INET, SOCK_STREAM, 0);
                if (srv == INVALID_SOCKET) {
                    send_evt(h_write, EVT_ERROR "socket() falhou");
                    continue;
                }

                /* SO_REUSEADDR evita "Address already in use" ao reiniciar rápido */
                int opt = 1;
                setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

                struct sockaddr_in addr = {0};
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port        = htons(NET_PORT);

                if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
                    listen(srv, 1) != 0) {
                    send_evt(h_write, EVT_ERROR "bind/listen falhou");
                    closesocket(srv);
                    srv = INVALID_SOCKET;
                    continue;
                }

                set_nonblocking(srv); /* accept não vai bloquear o loop */
                state = NET_LISTENING;
                send_evt(h_write, EVT_LISTENING);

            /* CMD: CONNECT:<ip> — conecta como cliente a outro peer */
            } else if (strncmp(cmd, CMD_CONNECT, strlen(CMD_CONNECT)) == 0 && state == NET_IDLE) {
                const char *ip = cmd + strlen(CMD_CONNECT);

                peer = socket(AF_INET, SOCK_STREAM, 0);
                if (peer == INVALID_SOCKET) {
                    send_evt(h_write, EVT_ERROR "socket() falhou");
                    continue;
                }

                struct sockaddr_in addr = {0};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(NET_PORT);
                inet_pton(AF_INET, ip, &addr.sin_addr);

                /* connect é bloqueante aqui — aceitável porque é chamado sob demanda */
                if (connect(peer, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                    _snprintf(evt, sizeof(evt), EVT_ERROR "connect falhou para %s", ip);
                    send_evt(h_write, evt);
                    closesocket(peer);
                    peer = INVALID_SOCKET;
                    continue;
                }

                set_nonblocking(peer);
                state = NET_CONNECTED;
                _snprintf(evt, sizeof(evt), EVT_CONNECTED "%s", ip);
                send_evt(h_write, evt);

            /* CMD: SEND:<texto> — envia mensagem ao peer conectado */
            } else if (strncmp(cmd, CMD_SEND, strlen(CMD_SEND)) == 0 && state == NET_CONNECTED) {
                const char *msg = cmd + strlen(CMD_SEND);
                send(peer, msg, (int)strlen(msg), 0);
            }
        }

        /* Modo servidor: tenta aceitar conexão de entrada */
        if (state == NET_LISTENING && srv != INVALID_SOCKET) {
            struct sockaddr_in cli_addr = {0};
            int cli_len = sizeof(cli_addr);
            SOCKET incoming = accept(srv, (struct sockaddr*)&cli_addr, &cli_len);

            if (incoming != INVALID_SOCKET) {
                peer = incoming;
                set_nonblocking(peer);

                char ip_str[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, sizeof(ip_str));

                closesocket(srv); /* aceita apenas um peer por sessão */
                srv = INVALID_SOCKET;

                state = NET_CONNECTED;
                _snprintf(evt, sizeof(evt), EVT_CONNECTED "%s", ip_str);
                send_evt(h_write, evt);
            }
        }

        /* Modo conectado: verifica se há mensagem recebida do peer */
        if (state == NET_CONNECTED && peer != INVALID_SOCKET) {
            int n = recv(peer, recv_buf, sizeof(recv_buf) - 1, 0);

            if (n > 0) {
                recv_buf[n] = '\0';
                _snprintf(evt, sizeof(evt), EVT_MSG "%s", recv_buf);
                send_evt(h_write, evt);

            } else if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
                /* peer encerrou a conexão */
                closesocket(peer);
                peer  = INVALID_SOCKET;
                state = NET_IDLE;
                send_evt(h_write, EVT_DISCONNECTED);
            }
            /* WSAEWOULDBLOCK = socket não-bloqueante sem dados: ignora */
        }
    }

    /* Limpeza */
    if (peer != INVALID_SOCKET) closesocket(peer);
    if (srv  != INVALID_SOCKET) closesocket(srv);
    WSACleanup();
    CloseHandle(h_read);
    CloseHandle(h_write);
}
