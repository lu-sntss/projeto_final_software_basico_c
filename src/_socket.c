#include <libwebsockets.h>
#include <string.h>

static int callback_ws(
    struct lws *wsi,
    enum lws_callback_reasons reason,
    void *user,
    void *in,
    size_t len)
{
    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED:
            printf("Cliente conectado\n");
            break;

        case LWS_CALLBACK_RECEIVE:
            printf("Recebido: %s\n", (char *)in);
            break;

        case LWS_CALLBACK_CLOSED:
            printf("Cliente desconectado\n");
            break;

        default:
            break;
    }

    return 0;
}

static struct lws_protocols protocols[] = {
    {
        .name = "teste",
        .callback = callback_ws,
        .per_session_data_size = 0,
        .rx_buffer_size = 4096,
        .id = 0,
        .user = NULL,
        .tx_packet_size = 0,
    },
    LWS_PROTOCOL_LIST_TERM
};

int main()
{
    struct lws_context_creation_info info;

    memset(&info, 0, sizeof(info));

    info.port = 9000;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DISABLE_IPV6;

    struct lws_context *context = lws_create_context(&info);

    if (!context)
    {
        printf("Erro ao criar servidor\n");
        return -1;
    }

    printf("Servidor WebSocket na porta 9000\n");

    while (lws_service(context, 0) >= 0);

    lws_context_destroy(context);

    return 0;
}