#ifndef NET_H
#define NET_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef NET_PORT
#define NET_PORT 7777  /* sobrescrito via -DNET_PORT=xxxx no build */
#endif

/* Comandos: UI → processo --net (via pipe) */
#define CMD_LISTEN   "LISTEN"    /* inicia servidor TCP na porta NET_PORT */
#define CMD_CONNECT  "CONNECT:"  /* conecta a um IP:  CONNECT:192.168.0.5 */
#define CMD_SEND     "SEND:"     /* envia mensagem:   SEND:texto aqui     */

/* Eventos: processo --net → UI (via pipe) */
#define EVT_LISTENING    "LISTENING"     /* servidor ativo, aguardando peer */
#define EVT_CONNECTED    "CONNECTED:"    /* conexão ok:  CONNECTED:192.168.0.5 */
#define EVT_MSG          "MSG:"          /* mensagem recebida: MSG:texto aqui  */
#define EVT_DISCONNECTED "DISCONNECTED"  /* peer encerrou a conexão */
#define EVT_ERROR        "ERROR:"        /* falha: ERROR:descrição */

/*
    Ponto de entrada do processo worker --net.
    Gerencia a conexão TCP e retransmite eventos para a UI pelo pipe.

    Args:
        h_read:  lê comandos vindos da UI.
        h_write: escreve eventos de volta para a UI.

    Returns: void. Encerra quando o pipe for fechado.
*/
void run_net(HANDLE h_read, HANDLE h_write);

#endif /* NET_H */
