/*
 * POC Chat - Win32 Nativo + Nuklear GDI
 *
 * Build (MinGW/GCC):
 *   gcc src/main.c -o poc_chat.exe -I./libs -lgdi32 -mwindows
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // Adicionado para lidar com argumentos de linha de comando
#include <wchar.h>    // Adicionado para conversão de strings
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#define NK_GDI_IMPLEMENTATION
#include "../libs/nuklear.h"
#include "../libs/nuklear_gdi.h"

/* =====================================================================
   BACKEND
   Invocado como processo filho: poc_chat.exe --backend <h_write> <h_read>
   ===================================================================== */

static void rodar_backend(HANDLE h_read, HANDLE h_write) {
    char buf[512];
    char resp[600];
    DWORD avail, lidos, escritos;

    while (1) {
        Sleep(100);

        avail = 0;
        if (!PeekNamedPipe(h_read, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) continue;

        if (ReadFile(h_read, buf, sizeof(buf) - 1, &lidos, NULL) && lidos > 0) {
            buf[lidos] = '\0';
            _snprintf(resp, sizeof(resp),
                      "[Sistema] Backend roteando pacote: %s\n", buf);
            WriteFile(h_write, resp, (DWORD)strlen(resp), &escritos, NULL);
        }
    }

    CloseHandle(h_read);
    CloseHandle(h_write);
}

/* =====================================================================
   UI  —  Nuklear GDI
   ===================================================================== */

#define WIN_W   600
#define WIN_H   420
#define LOG_MAX 8192

static struct nk_context *ctx;
static GdiFont            *font;

static char log_chat[LOG_MAX];
static int  log_len = 0;

static char id_buf[64];
static int  id_len = 0;
static char msg_buf[256];
static int  msg_len = 0;

static HANDLE g_read;
static HANDLE g_write;

static void log_append(const char *text) {
    int n = (int)strlen(text);
    if (log_len + n + 1 > LOG_MAX) {
        /* Descarta a primeira metade para liberar espaço */
        int metade = LOG_MAX / 2;
        memmove(log_chat, log_chat + metade, log_len - metade);
        log_len -= metade;
    }
    int copiar = n;
    if (log_len + copiar + 1 > LOG_MAX) copiar = LOG_MAX - log_len - 1;
    if (copiar > 0) {
        memcpy(log_chat + log_len, text, copiar);
        log_len += copiar;
        log_chat[log_len] = '\0';
    }
}

/* Chamado a cada frame: lê o pipe do backend sem bloquear a UI */
static void poll_backend(void) {
    DWORD avail = 0;
    if (!PeekNamedPipe(g_read, NULL, 0, NULL, &avail, NULL) || avail == 0)
        return;

    char buf[512];
    DWORD lidos = 0;
    if (ReadFile(g_read, buf, sizeof(buf) - 1, &lidos, NULL) && lidos > 0) {
        buf[lidos] = '\0';
        log_append(buf);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (nk_gdi_handle_event(hwnd, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int rodar_ui(HINSTANCE hInst, HANDLE h_read, HANDLE h_write) {
    g_read  = h_read;
    g_write = h_write;

    /* Janela Win32 nativa */
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

    MSG msg_win;
    while (1) {
        /* Processa eventos Win32 e repassa ao Nuklear */
        nk_input_begin(ctx);
        while (PeekMessageW(&msg_win, NULL, 0, 0, PM_REMOVE)) {
            if (msg_win.message == WM_QUIT) goto encerrar;
            TranslateMessage(&msg_win);
            DispatchMessageW(&msg_win);
        }
        nk_input_end(ctx);

        /* Lê dados do backend sem bloquear */
        poll_backend();

        /* ── Layout Nuklear ── */
        if (nk_begin(ctx, "Chat",
                     nk_rect(0, 0, WIN_W, WIN_H),
                     NK_WINDOW_NO_SCROLLBAR)) {

            /* Área de log — caixa de texto rolável e somente leitura */
            nk_layout_row_dynamic(ctx, WIN_H - 100, 1);
            nk_edit_string_zero_terminated(
                ctx,
                NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                log_chat, LOG_MAX,
                nk_filter_default);

            /* Linha de controles: [ID destinatário] [Mensagem] [Enviar] */
            nk_layout_row_begin(ctx, NK_STATIC, 32, 3);

            nk_layout_row_push(ctx, 90);
            nk_edit_string(ctx, NK_EDIT_SIMPLE,
                           id_buf, &id_len, (int)sizeof(id_buf) - 1,
                           nk_filter_default);

            nk_layout_row_push(ctx, WIN_W - 90 - 92 - 24);
            nk_edit_string(ctx, NK_EDIT_SIMPLE,
                           msg_buf, &msg_len, (int)sizeof(msg_buf) - 1,
                           nk_filter_default);

            nk_layout_row_push(ctx, 82);
            if (nk_button_label(ctx, "Enviar")) {
                if (id_len > 0 && msg_len > 0) {
                    id_buf[id_len]   = '\0';
                    msg_buf[msg_len] = '\0';

                    /* Despacha pacote para o backend via WriteFile */
                    char pacote[512];
                    _snprintf(pacote, sizeof(pacote),
                              "%s -> %s", id_buf, msg_buf);
                    DWORD wr;
                    WriteFile(g_write, pacote,
                              (DWORD)strlen(pacote), &wr, NULL);

                    /* Eco local imediato */
                    char eco[600];
                    _snprintf(eco, sizeof(eco),
                              "Você (para %s): %s\n", id_buf, msg_buf);
                    log_append(eco);

                    /* Limpa campo de mensagem */
                    memset(msg_buf, 0, sizeof(msg_buf));
                    msg_len = 0;
                }
            }

            nk_layout_row_end(ctx);
        }
        nk_end(ctx);

        nk_gdi_render(nk_rgb(25, 25, 30));
        Sleep(16); /* ~60 FPS */
    }

encerrar:
    nk_gdifont_del(font);
    nk_gdi_shutdown();
    ReleaseDC(hwnd, dc);
    return 0;
}

/* =====================================================================
   PONTO DE ENTRADA
   ===================================================================== */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* ── Modo backend ── */
    if (argc >= 4 && lstrcmpW(argv[1], L"--backend") == 0) {
        HANDLE h_read  = (HANDLE)(UINT_PTR)wcstoull(argv[2], NULL, 10);
        HANDLE h_write = (HANDLE)(UINT_PTR)wcstoull(argv[3], NULL, 10);
        LocalFree(argv);
        rodar_backend(h_read, h_write);
        return 0;
    }
    LocalFree(argv);

    /* ── Modo UI: cria pipes e lança processo filho (backend) ── */
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE btu_r, btu_w;  /* back_to_ui:  backend escreve, UI lê  */
    HANDLE utb_r, utb_w;  /* ui_to_back:  UI escreve, backend lê  */

    if (!CreatePipe(&btu_r, &btu_w, &sa, 0) ||
        !CreatePipe(&utb_r, &utb_w, &sa, 0)) {
        MessageBoxW(NULL, L"Falha ao criar pipes IPC.", L"Erro", MB_ICONERROR);
        return 1;
    }

    /* Pontas que ficam no processo pai NÃO devem ser herdadas pelo filho */
    SetHandleInformation(btu_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(utb_w, HANDLE_FLAG_INHERIT, 0);

    /* Monta linha de comando: "<exe> --backend <btu_w> <utb_r>" */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    wchar_t cmd[MAX_PATH + 128];
    _snwprintf(cmd, _countof(cmd),
               L"\"%s\" --backend %I64u %I64u",
               exe,
               (unsigned __int64)(UINT_PTR)btu_w,
               (unsigned __int64)(UINT_PTR)utb_r);

    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessW(NULL, cmd, NULL, NULL,
                        TRUE,  /* bInheritHandles */
                        0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"Falha ao iniciar processo backend.", L"Erro", MB_ICONERROR);
        return 1;
    }

    /* Fecha as pontas do filho que estão no processo pai */
    CloseHandle(btu_w);
    CloseHandle(utb_r);

    /* Roda a interface gráfica */
    int resultado = rodar_ui(hInst, btu_r, utb_w);

    /* Encerra o processo backend e libera recursos */
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(btu_r);
    CloseHandle(utb_w);

    return resultado;
}
