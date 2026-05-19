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
        "ws-protocol",
        callback_ws,
        0,
        4096,
    },
    { NULL, NULL, 0, 0 }
};

int main()
{
    struct lws_context_creation_info info;

    memset(&info, 0, sizeof(info));

    info.port = 9000;
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);

    if (!context)
    {
        printf("Erro ao criar servidor\n");
        return -1;
    }

    printf("Servidor WebSocket na porta 9000\n");

    while (1)
    {
        lws_service(context, 100);
    }

    lws_context_destroy(context);

    return 0;
}