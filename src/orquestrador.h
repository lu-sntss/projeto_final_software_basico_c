#ifndef ORQUESTRADOR_H
#define ORQUESTRADOR_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Número máximo de processos workers rodando ao mesmo tempo.
   Backend + Rede (futuro) já cabem com folga. */
#define MAX_WORKERS 4

/*
 * Worker representa um processo filho gerenciado pelo orquestrador.
 * Toda comunicação com ele acontece pelos dois pipes.
 *
 * Cada novo worker futuro (ex: --net para sockets LAN) vai seguir
 * exatamente essa mesma estrutura — a UI não precisa saber o que
 * o worker faz por dentro, só conversa com ele pelos pipes.
 */
typedef struct {
    HANDLE            pipe_read;  /* UI lê respostas do worker aqui    */
    HANDLE            pipe_write; /* UI escreve comandos pro worker aqui */
    PROCESS_INFORMATION proc_info; /* PID e handles do processo filho   */
    int               active;     /* 1 = rodando, 0 = slot livre        */
} Worker;

/* Registro global de todos os workers ativos. */
extern Worker workers[MAX_WORKERS];

/*
    Lança um novo processo worker do tipo especificado.

    Cria os dois pipes, remove herança das pontas do pai e chama
    CreateProcess passando os handles do filho na linha de comando.

    Args:
        type: argumento que identifica o worker, ex: L"--backend", L"--net"

    Returns:
        Índice em workers[] se bem-sucedido, ou -1 em caso de falha.
*/
int orq_launch(const wchar_t *type);

/*
    Encerra todos os workers ativos e libera seus recursos.

    TerminateProcess + WaitForSingleObject em cada worker ativo,
    fecha todos os handles e zera o slot para reutilização.

    Returns: void.
*/
void orq_shutdown_all(void);

#endif /* ORQUESTRADOR_H */
