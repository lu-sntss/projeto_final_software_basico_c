#include "orquestrador.h"
#include <stdio.h>

/* Array global com os slots de workers. Começa todo zerado/inativo. */
Worker workers[MAX_WORKERS] = {0};


/*
    Procura o primeiro slot livre no array de workers.
    Returns: índice do slot livre, ou -1 se todos estiverem ocupados.
*/
static int find_free_slot(void) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!workers[i].active) return i;
    }
    return -1;
}


int orq_launch(const wchar_t *type) {
    int slot = find_free_slot();
    if (slot == -1) {
        MessageBoxW(NULL, L"Limite de workers atingido.", L"Orquestrador", MB_ICONWARNING);
        return -1;
    }

    /* Cria os dois pipes com herança habilitada.
       O filho vai herdar apenas as pontas dele — as do pai são bloqueadas logo abaixo. */
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE btu_r, btu_w; /* back_to_ui:  worker escreve → UI lê   */
    HANDLE utb_r, utb_w; /* ui_to_back:  UI escreve   → worker lê */

    if (!CreatePipe(&btu_r, &btu_w, &sa, 0) ||
        !CreatePipe(&utb_r, &utb_w, &sa, 0)) {
        MessageBoxW(NULL, L"Falha ao criar pipes do worker.", L"Orquestrador", MB_ICONERROR);
        return -1;
    }

    /* Remove herança das pontas que ficam no processo pai.
       Sem isso o filho herdaria os 4 handles, impedindo o pipe de fechar corretamente. */
    SetHandleInformation(btu_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(utb_w, HANDLE_FLAG_INHERIT, 0);

    /* Serializa as pontas do filho na linha de comando.
       Ordem: argv[2] = utb_r (filho lê daqui), argv[3] = btu_w (filho escreve aqui).
       As aspas em torno do caminho protegem caminhos com espaços. */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    wchar_t cmd[MAX_PATH + 128];
    _snwprintf(cmd, _countof(cmd), L"\"%s\" %s %I64u %I64u",
               exe,
               type,
               (unsigned __int64)(UINT_PTR)utb_r,  /* argv[2]: filho lê daqui    */
               (unsigned __int64)(UINT_PTR)btu_w);  /* argv[3]: filho escreve aqui */

    STARTUPINFOW si = {sizeof(si)};

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL,
                        &si, &workers[slot].proc_info)) {
        MessageBoxW(NULL, L"Falha ao criar processo worker.", L"Orquestrador", MB_ICONERROR);
        CloseHandle(btu_r); CloseHandle(btu_w);
        CloseHandle(utb_r); CloseHandle(utb_w);
        return -1;
    }

    /* Fecha as cópias das pontas do filho que ficaram abertas no pai.
       O pipe só sinaliza EOF quando todos os handles para ele forem fechados. */
    CloseHandle(btu_w);
    CloseHandle(utb_r);

    /* Registra o worker no slot e marca como ativo. */
    workers[slot].pipe_read  = btu_r; /* UI lê respostas do worker  */
    workers[slot].pipe_write = utb_w; /* UI escreve comandos pro worker */
    workers[slot].active     = 1;

    return slot;
}


void orq_shutdown_all(void) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!workers[i].active) continue;

        TerminateProcess(workers[i].proc_info.hProcess, 0); /* equivale a kill(SIGKILL) */
        WaitForSingleObject(workers[i].proc_info.hProcess, 3000); /* equivale a waitpid */

        CloseHandle(workers[i].proc_info.hProcess);
        CloseHandle(workers[i].proc_info.hThread);
        CloseHandle(workers[i].pipe_read);
        CloseHandle(workers[i].pipe_write);

        workers[i] = (Worker){0}; /* limpa o slot para reutilização */
    }
}
