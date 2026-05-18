/*
 * POC Chat - Win32 Nativo + Nuklear GDI
 *
 * ARQUITETURA: executável único, três modos.
 *   [UI]        lança workers via orquestrador, exibe a janela.
 *   [--backend] processo de echo simulado (stub).
 *   [--net]     processo de sockets LAN via Winsock2.
 *
 * POSIX → Win32: fork→CreateProcess, pipe→CreatePipe,
 *                fcntl O_NONBLOCK→PeekNamedPipe, kill/waitpid→TerminateProcess/WaitForSingleObject
 *
 * Build: gcc src/main.c src/orquestrador.c src/net.c -o poc_chat.exe -I./libs -lgdi32 -lshell32 -lmsimg32 -lws2_32 -mwindows
 *
  gcc src/main.c src/orquestrador.c src/net.c -o poc_chat_7777.exe -I./libs -lgdi32 -lshell32
  -lmsimg32 -lws2_32 -mwindows -static-libgcc -DNET_PORT=7777

  gcc src/main.c src/orquestrador.c src/net.c -o poc_chat_5000.exe -I./libs -lgdi32 -lshell32
  -lmsimg32 -lws2_32 -mwindows -static-libgcc -DNET_PORT=5000

  gcc src/main.c src/orquestrador.c src/net.c -o poc_chat_8080.exe -I./libs -lgdi32 -lshell32
  -lmsimg32 -lws2_32 -mwindows -static-libgcc -DNET_PORT=8080
 *
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#define NK_GDI_IMPLEMENTATION
#include "../libs/nuklear.h"
#include "../libs/nuklear_gdi.h"

#include "orquestrador.h"
#include "net.h"


/*
    Loop do processo --backend. Lê mensagens da UI e devolve uma resposta simulada.
*/
static void run_backend(HANDLE h_read, HANDLE h_write) {
    char buf[512];
    char resp[600];
    DWORD avail, bytes_read, bytes_written;

    while (1) {
        Sleep(100);

        avail = 0;
        if (!PeekNamedPipe(h_read, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) continue;

        if (ReadFile(h_read, buf, sizeof(buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
            buf[bytes_read] = '\0';
            _snprintf(resp, sizeof(resp), "[Sistema] Backend roteando pacote: %s\n", buf);
            WriteFile(h_write, resp, (DWORD)strlen(resp), &bytes_written, NULL);
        }
    }

    CloseHandle(h_read);
    CloseHandle(h_write);
}


#define WIN_W   640   /* largura da área útil da janela em pixels */
#define WIN_H   480   /* altura da área útil da janela em pixels  */
#define LOG_MAX 8192  /* tamanho máximo do buffer de texto do chat */

static struct nk_context *ctx;
static GdiFont            *font;

/* Buffer de log do chat */
static char chat_log[LOG_MAX];
static int  chat_log_len = 0;

/* Campos de entrada da UI */
static char id_buf[64];
static int  id_len = 0;
static char msg_buf[256];
static int  msg_len = 0;

/* Pipes do backend (echo simulado) */
static HANDLE g_read;
static HANDLE g_write;

/* Pipes do worker de rede */
static HANDLE g_net_read;
static HANDLE g_net_write;

/* IP digitado para conexão + estado exibido na barra de status */
static char ip_buf[64]     = "192.168.0.";
static int  ip_len         = 9;
static char net_status[64] = "Desconectado";


/*
    Concatena texto ao log descartando a metade mais antiga quando cheio.
*/
static void log_append(const char *text) {
    int n = (int)strlen(text);

    if (chat_log_len + n + 1 > LOG_MAX) {
        int half = LOG_MAX / 2;
        memmove(chat_log, chat_log + half, chat_log_len - half);
        chat_log_len -= half;
    }

    int to_copy = n;
    if (chat_log_len + to_copy + 1 > LOG_MAX) to_copy = LOG_MAX - chat_log_len - 1;

    if (to_copy > 0) {
        memcpy(chat_log + chat_log_len, text, to_copy);
        chat_log_len += to_copy;
        chat_log[chat_log_len] = '\0';
    }
}


/*
    Lê respostas do worker --backend sem bloquear. Chamada a cada frame.
*/
static void poll_backend(void) {
    DWORD avail = 0;
    if (!PeekNamedPipe(g_read, NULL, 0, NULL, &avail, NULL) || avail == 0)
        return;

    char buf[512];
    DWORD bytes_read = 0;
    if (ReadFile(g_read, buf, sizeof(buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
        buf[bytes_read] = '\0';
        log_append(buf);
    }
}


/*
    Lê eventos do worker --net sem bloquear. Atualiza log e status conforme o evento.
*/
static void poll_net(void) {
    DWORD avail = 0;
    if (!PeekNamedPipe(g_net_read, NULL, 0, NULL, &avail, NULL) || avail == 0)
        return;

    char buf[512];
    DWORD bytes_read = 0;
    if (!ReadFile(g_net_read, buf, sizeof(buf) - 1, &bytes_read, NULL) || bytes_read == 0)
        return;

    buf[bytes_read] = '\0';

    /* Interpreta o prefixo do evento e atualiza estado da UI */
    if (strcmp(buf, EVT_LISTENING) == 0) {
        strncpy(net_status, "Aguardando...", sizeof(net_status) - 1);
        log_append("[Rede] Servidor ativo, aguardando conexao.\n");

    } else if (strncmp(buf, EVT_CONNECTED, strlen(EVT_CONNECTED)) == 0) {
        const char *ip = buf + strlen(EVT_CONNECTED);
        _snprintf(net_status, sizeof(net_status), "Conectado: %s", ip);
        char line[128];
        _snprintf(line, sizeof(line), "[Rede] Conectado a %s.\n", ip);
        log_append(line);

    } else if (strncmp(buf, EVT_MSG, strlen(EVT_MSG)) == 0) {
        const char *msg = buf + strlen(EVT_MSG);
        char line[600];
        _snprintf(line, sizeof(line), "Peer: %s\n", msg);
        log_append(line);

    } else if (strcmp(buf, EVT_DISCONNECTED) == 0) {
        strncpy(net_status, "Desconectado", sizeof(net_status) - 1);
        log_append("[Rede] Peer encerrou a conexao.\n");

    } else if (strncmp(buf, EVT_ERROR, strlen(EVT_ERROR)) == 0) {
        const char *err = buf + strlen(EVT_ERROR);
        char line[600];
        _snprintf(line, sizeof(line), "[Rede] Erro: %s\n", err);
        log_append(line);
    }
}


/*
    Window Procedure: callback de eventos Win32.
*/
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (nk_gdi_handle_event(hwnd, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/*
    Inicializa a janela Win32 + Nuklear e executa o loop principal da UI.
*/
static int run_ui(HINSTANCE hInst, HANDLE h_read, HANDLE h_write,
                                   HANDLE h_net_read, HANDLE h_net_write) {
    g_read      = h_read;
    g_write     = h_write;
    g_net_read  = h_net_read;
    g_net_write = h_net_write;

    WNDCLASSW wc = {0};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"POCChat";
    RegisterClassW(&wc);

    RECT r = {0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, L"POCChat", L"POC Chat \x2014 Win32 + Nuklear GDI",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);

    HDC dc = GetDC(hwnd);
    font = nk_gdifont_create("Consolas", 14);
    ctx  = nk_gdi_init(font, dc, WIN_W, WIN_H);

    /* ip_len inicializado com o comprimento do valor padrão */
    ip_len = (int)strlen(ip_buf);

    MSG win_msg;
    while (1) {
        nk_input_begin(ctx);
        while (PeekMessageW(&win_msg, NULL, 0, 0, PM_REMOVE)) {
            if (win_msg.message == WM_QUIT) goto shutdown;
            TranslateMessage(&win_msg);
            DispatchMessageW(&win_msg);
        }
        nk_input_end(ctx);

        poll_backend();
        poll_net();

        if (nk_begin(ctx, "Chat", nk_rect(0, 0, WIN_W, WIN_H), NK_WINDOW_NO_SCROLLBAR)) {

            /* ── Painel de rede ── */
            nk_layout_row_begin(ctx, NK_STATIC, 28, 4);

            /* Rótulo de status da conexão */
            nk_layout_row_push(ctx, 160);
            nk_label(ctx, net_status, NK_TEXT_LEFT);

            /* Campo IP para conexão */
            nk_layout_row_push(ctx, 140);
            nk_edit_string(ctx, NK_EDIT_SIMPLE, ip_buf, &ip_len,
                           (int)sizeof(ip_buf) - 1, nk_filter_default);

            /* Botão Aguardar: envia LISTEN para o worker de rede */
            nk_layout_row_push(ctx, 90);
            if (nk_button_label(ctx, "Aguardar")) {
                DWORD w;
                WriteFile(g_net_write, CMD_LISTEN, (DWORD)strlen(CMD_LISTEN), &w, NULL);
            }

            /* Botão Conectar: envia CONNECT:<ip> para o worker de rede */
            nk_layout_row_push(ctx, 90);
            if (nk_button_label(ctx, "Conectar")) {
                ip_buf[ip_len] = '\0';
                char cmd[128];
                _snprintf(cmd, sizeof(cmd), CMD_CONNECT "%s", ip_buf);
                DWORD w;
                WriteFile(g_net_write, cmd, (DWORD)strlen(cmd), &w, NULL);
            }

            nk_layout_row_end(ctx);

            /* ── Separador visual ── */
            nk_layout_row_dynamic(ctx, 1, 1);
            nk_rule_horizontal(ctx, nk_rgb(60, 60, 70), nk_false);

            /* ── Área de log: scroll interno, somente leitura ── */
            nk_layout_row_dynamic(ctx, WIN_H - 28 - 1 - 40 - 36, 1);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                                           chat_log, LOG_MAX, nk_filter_default);

            /* ── Linha de envio ── */
            nk_layout_row_begin(ctx, NK_STATIC, 32, 2);

            nk_layout_row_push(ctx, WIN_W - 92 - 16); /* campo mensagem */
            nk_edit_string(ctx, NK_EDIT_SIMPLE, msg_buf, &msg_len,
                           (int)sizeof(msg_buf) - 1, nk_filter_default);

            nk_layout_row_push(ctx, 82); /* botão enviar */
            if (nk_button_label(ctx, "Enviar")) {
                if (msg_len > 0) {
                    msg_buf[msg_len] = '\0';

                    /* Envia pelo worker de rede */
                    char net_cmd[300];
                    _snprintf(net_cmd, sizeof(net_cmd), CMD_SEND "%s", msg_buf);
                    DWORD w;
                    WriteFile(g_net_write, net_cmd, (DWORD)strlen(net_cmd), &w, NULL);

                    /* Exibe localmente sem esperar confirmação */
                    char echo[300];
                    _snprintf(echo, sizeof(echo), "Você: %s\n", msg_buf);
                    log_append(echo);

                    memset(msg_buf, 0, sizeof(msg_buf));
                    msg_len = 0;
                }
            }

            nk_layout_row_end(ctx);
        }
        nk_end(ctx);

        nk_gdi_render(nk_rgb(25, 25, 30));
        Sleep(16);
    }

shutdown:
    nk_gdifont_del(font);
    nk_gdi_shutdown();
    ReleaseDC(hwnd, dc);
    return 0;
}


/*
    Ponto de entrada. Detecta o modo e delega ao módulo correto.
*/
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* Modo --backend */
    if (argc >= 4 && lstrcmpW(argv[1], L"--backend") == 0) {
        HANDLE h_read  = (HANDLE)(UINT_PTR)wcstoull(argv[2], NULL, 10);
        HANDLE h_write = (HANDLE)(UINT_PTR)wcstoull(argv[3], NULL, 10);
        LocalFree(argv);
        run_backend(h_read, h_write);
        return 0;
    }

    /* Modo --net */
    if (argc >= 4 && lstrcmpW(argv[1], L"--net") == 0) {
        HANDLE h_read  = (HANDLE)(UINT_PTR)wcstoull(argv[2], NULL, 10);
        HANDLE h_write = (HANDLE)(UINT_PTR)wcstoull(argv[3], NULL, 10);
        LocalFree(argv);
        run_net(h_read, h_write);
        return 0;
    }

    LocalFree(argv);

    /* Modo UI: lança os dois workers */
    int backend_slot = orq_launch(L"--backend");
    if (backend_slot == -1) return 1;

    int net_slot = orq_launch(L"--net");
    if (net_slot == -1) {
        orq_shutdown_all();
        return 1;
    }

    int result = run_ui(hInst,
                        workers[backend_slot].pipe_read,
                        workers[backend_slot].pipe_write,
                        workers[net_slot].pipe_read,
                        workers[net_slot].pipe_write);

    orq_shutdown_all();
    return result;
}
