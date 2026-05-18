/*
 * =============================================================================
 * POC Chat - Win32 Nativo + Nuklear GDI
 * =============================================================================
 *
 * ARQUITETURA GERAL
 * -----------------
 * Este programa opera em DOIS MODOS dentro de um único executável:
 *
 *   [MODO UI]      (padrão, sem argumentos)
 *   Cria os pipes de comunicação, lança uma segunda instância de si mesmo
 *   como processo filho (o backend), e exibe a janela de chat via Nuklear.
 *
 *   [MODO BACKEND] (invocado com --backend <handle1> <handle2>)
 *   Fica em loop lendo mensagens da UI pelo pipe e devolvendo respostas.
 *   Não tem interface gráfica, roda silenciosamente em background.
 *
 * SUBSTITUTOS DOS CONCEITOS POSIX
 * ---------------------------------
 *   fork()           →  CreateProcess() invocando o próprio executável
 *   pipe() + fd[2]   →  CreatePipe() retornando HANDLEs
 *   fcntl O_NONBLOCK →  PeekNamedPipe() antes de ReadFile()
 *   kill() + waitpid →  TerminateProcess() + WaitForSingleObject()
 *   usleep()         →  Sleep()
 *   Biblioteca IUP   →  Nuklear (header-only, backend GDI nativo do Windows)
 *
 * Build (MinGW/GCC):
 *   gcc src/main.c -o poc_chat.exe -I./libs -lgdi32 -lshell32 -lmsimg32 -mwindows
 * =============================================================================
 */

/* WIN32_LEAN_AND_MEAN instrui o windows.h a não incluir cabeçalhos secundários
   raramente usados (COM, RPC, Winsock antigo etc.), reduzindo o tempo de
   compilação sem perder nada do que precisamos. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> /* CommandLineToArgvW: converte a linha de comando em argv[] */
#include <wchar.h>    /* wcstoull: converte string wide-char para inteiro (para os handles) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * CONFIGURAÇÃO DA BIBLIOTECA NUKLEAR
 * ------------------------------------
 * O Nuklear é header-only: todo o código fonte está dentro do nuklear.h.
 * Para ativá-lo você define macros ANTES do #include. Cada macro habilita
 * um módulo específico:
 *
 *   NK_INCLUDE_FIXED_TYPES      → garante tipos de tamanho fixo (nk_uint32, etc.)
 *   NK_INCLUDE_STANDARD_IO      → habilita funções de I/O padrão internamente
 *   NK_INCLUDE_STANDARD_VARARGS → habilita suporte a funções variádicas (printf-like)
 *   NK_INCLUDE_DEFAULT_ALLOCATOR → usa malloc/free internamente para alocações
 *   NK_IMPLEMENTATION           → instrui o header a COMPILAR o corpo das funções
 *                                  (sem isso, só as declarações são incluídas)
 *
 * NK_GDI_IMPLEMENTATION faz o mesmo para o backend GDI do Windows (nuklear_gdi.h).
 * Esse backend traduz as primitivas do Nuklear (retângulos, texto, linhas) em
 * chamadas nativas do GDI32 — sem OpenGL, sem SDL, sem dependências externas.
 *
 * IMPORTANTE: NK_IMPLEMENTATION e NK_GDI_IMPLEMENTATION devem ser definidos
 * em APENAS UM arquivo .c do projeto. Em projetos com múltiplos .c, colocá-los
 * em mais de um arquivo causaria erro de símbolo duplicado no linker.
 */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#define NK_GDI_IMPLEMENTATION
#include "../libs/nuklear.h"
#include "../libs/nuklear_gdi.h"


/* =============================================================================
   SEÇÃO 1 — BACKEND
   =============================================================================
   O backend é o "serviço de rede" do chat. No Linux, ele seria o processo
   filho criado pelo fork(). No Windows, ele é o mesmo executável relançado
   com o argumento --backend.

   Ele NÃO tem janela. Fica em loop verificando o pipe que vem da UI,
   processa a mensagem recebida e manda uma resposta de volta.
   ============================================================================= */

/*
    Loop principal do processo de backend (serviço de mensageria).

    Esta função é o coração do processo filho. Ela substitui o comportamento
    que no Linux era feito pelo processo filho após o fork(). Como o Windows
    não tem fork() — que clona o estado inteiro da memória —, o pai relança
    o próprio executável passando as referências dos pipes pela linha de comando.

    O loop usa PeekNamedPipe + ReadFile em vez de fcntl(O_NONBLOCK) + read().
    No Linux, O_NONBLOCK fazia read() retornar imediatamente com -1 (EAGAIN)
    quando não havia dados. No Windows, o equivalente é PeekNamedPipe(), que
    inspeciona o pipe sem consumir os dados e informa quantos bytes estão
    disponíveis — se for zero, simplesmente pulamos a leitura.

    Args:
        h_read:  HANDLE do pipe de leitura (mensagens que vêm da UI).
                 Equivale à ponta de leitura do pipe_ui_to_back do Linux.
        h_write: HANDLE do pipe de escrita (respostas enviadas para a UI).
                 Equivale à ponta de escrita do pipe_back_to_ui do Linux.

    Returns:
        void. A função só retorna quando o pipe for fechado pelo processo pai
        (o que acontece quando o usuário fecha a janela de chat).
*/
static void run_backend(HANDLE h_read, HANDLE h_write) {
    char buf[512];   /* buffer para ler a mensagem que chega da UI */
    char response[600];  /* buffer para montar a resposta que vai para a UI */
    DWORD disponivel, lidos, escritos;

    while (1) {
        /* Sleep(100) = usleep(100000) do Linux.
           Pausa o processo por 100 milissegundos entre cada verificação.
           Sem isso, o loop consumiria 100% de um núcleo de CPU sem necessidade
           (busy-waiting). É o equivalente ao "throttle" do daemon. */
        Sleep(100);

        /* PeekNamedPipe — o coração da leitura não-bloqueante no Windows
           ----------------------------------------------------------------
           No Linux usávamos: fcntl(fd, F_SETFL, O_NONBLOCK)
           Isso fazia com que read() retornasse -1 imediatamente se o pipe
           estivesse vazio, em vez de bloquear o processo.

           No Windows não existe esse conceito de "modo não-bloqueante" em pipes.
           A alternativa é PeekNamedPipe(), que:
             1. Olha dentro do pipe SEM remover os dados
             2. Preenche &avail com o número de bytes disponíveis para leitura
             3. Retorna FALSE se o pipe foi fechado pelo outro lado (erro)

           Assinatura:
             PeekNamedPipe(
               hNamedPipe,        → o pipe que queremos inspecionar
               lpBuffer,          → buffer para copiar dados (passamos NULL: só queremos o count)
               nBufferSize,       → tamanho do buffer acima (0, pois não copiamos)
               lpBytesRead,       → bytes copiados para lpBuffer (NULL, não usamos)
               lpTotalBytesAvail, → AQUI: quantos bytes estão no pipe agora
               lpBytesLeftThisMsg → bytes restantes nesta mensagem (NULL, não usamos)
             )
        */
        disponivel = 0;
        if (!PeekNamedPipe(h_read, NULL, 0, NULL, &disponivel, NULL)) {
            /* PeekNamedPipe retornou FALSE: o pipe foi fechado.
               Isso acontece quando o processo pai (UI) encerrou e fechou
               sua ponta do pipe. Saímos do loop normalmente. */
            break;
        }
        if (disponivel == 0) {
            /* Pipe está vazio. Não há mensagem para processar agora.
               Voltamos ao início do loop para aguardar mais 100ms. */
            continue;
        }

        /* Há dados disponíveis. Agora sim podemos chamar ReadFile sem risco
           de bloquear o processo, pois já sabemos que tem bytes no pipe.

           ReadFile no Windows é o equivalente do read() do Linux para pipes.
           Diferença principal: em vez de um int (fd), usa um HANDLE.

           Assinatura relevante:
             ReadFile(
               hFile,                → o pipe de onde vamos ler
               lpBuffer,             → buffer de destino
               nNumberOfBytesToRead, → máximo de bytes que queremos ler
               lpNumberOfBytesRead,  → quanto foi realmente lido (saída)
               lpOverlapped          → NULL = operação síncrona (o mais simples)
             )
        */
        if (ReadFile(h_read, buf, sizeof(buf) - 1, &lidos, NULL) && lidos > 0) {
            buf[lidos] = '\0'; /* garante terminador de string */

            /* Monta a resposta do backend simulando o roteamento da mensagem */
            _snprintf(response, sizeof(response),
                      "[Sistema] Backend roteando pacote: %s\n", buf);

            /* WriteFile envia a resposta de volta para a UI pelo outro pipe.
               É o equivalente do write() do Linux. Mesmo princípio: HANDLE
               em vez de file descriptor, e lpNumberOfBytesWritten em vez
               de valor de retorno. */
            WriteFile(h_write, response, (DWORD)strlen(response), &escritos, NULL);
        }
    }

    /* Libera os HANDLEs quando o loop termina.
       No Linux, o processo filho herdava os file descriptors e eles eram
       fechados automaticamente no exit(). No Windows o comportamento é
       similar, mas fechar explicitamente é boa prática. */
    CloseHandle(h_read);
    CloseHandle(h_write);
}


/* =============================================================================
   SEÇÃO 2 — INTERFACE GRÁFICA (Nuklear GDI)
   =============================================================================
   Toda esta seção é responsável pela janela de chat. No Linux, usávamos a
   biblioteca IUP com callbacks e timers. Aqui, o Nuklear usa um paradigma
   completamente diferente chamado Immediate Mode GUI (IMGUI):

   RETAINED MODE (IUP, Qt, GTK, WinForms):
     Você CRIA widgets uma vez (IupButton, IupText...) e eles ficam na memória.
     Quando o estado muda, você atualiza os atributos dos objetos.
     A biblioteca gerencia quais widgets existem e quando redesenhar.

   IMMEDIATE MODE (Nuklear):
     Você NÃO cria objetos persistentes. A cada frame (a cada ~16ms), você
     REDESCREVE a UI inteira do zero chamando funções como nk_button_label().
     Se o botão foi clicado, a função retorna verdadeiro naquele frame.
     Se o campo de texto mudou, o buffer externo (id_buf, msg_buf) é atualizado.
     Não há callbacks: a lógica de negócio fica dentro do próprio if().

   Isso pode parecer ineficiente, mas é extremamente simples de implementar
   e perfeito para aplicações em tempo real que redesenham tudo o tempo todo.
   ============================================================================= */

#define WIN_W   600   /* largura da janela em pixels */
#define WIN_H   420   /* altura da janela em pixels  */
#define LOG_MAX 8192  /* tamanho máximo do buffer de log do chat */

/*
 * ctx  — ponteiro para o contexto do Nuklear.
 *        É o "objeto central" da biblioteca: guarda o estado de todos os
 *        widgets, o estado de input (teclado/mouse) e as comandos de desenho
 *        pendentes para o frame atual.
 *
 * font — ponteiro para a fonte GDI que o Nuklear vai usar para renderizar texto.
 *        O nuklear_gdi.h implementa fontes usando a API CreateFont do Windows,
 *        evitando o sistema de atlas/baking que versões OpenGL precisariam.
 */
static struct nk_context *ctx;
static GdiFont            *font;

/*
 * log_chat — buffer circular de texto que exibe o histórico do chat.
 *            Ao contrário de uma lista de strings, mantemos tudo num único
 *            char[] para simplicidade. log_len rastreia quantos bytes válidos
 *            estão presentes (é diferente de strlen quando descartamos conteúdo).
 */
static char log_chat[LOG_MAX];
static int  log_len = 0;

/*
 * id_buf / msg_buf — buffers de texto para os campos de entrada da UI.
 *                    O Nuklear não guarda o conteúdo dos campos internamente:
 *                    ele lê e escreve DIRETAMENTE nesses arrays externos a
 *                    cada frame. id_len e msg_len rastreiam quantos caracteres
 *                    o usuário digitou até agora.
 */
static char id_buf[64];
static int  id_len = 0;
static char msg_buf[256];
static int  msg_len = 0;

/*
 * g_read / g_write — HANDLEs globais dos pipes usados pela UI.
 *   g_read:  a UI lê DAQUI as mensagens que chegam do backend.
 *   g_write: a UI ESCREVE AQUI as mensagens que o usuário envia.
 *
 * São globais porque poll_backend() precisa acessá-los a cada frame,
 * mas WndProc não pode receber parâmetros adicionais (é uma callback Win32).
 */
static HANDLE g_read;
static HANDLE g_write;


/*
    Adiciona texto ao buffer de log do chat com descarte circular.

    Cada mensagem nova (eco do usuário ou resposta do backend) é concatenada
    ao final de log_chat. Quando o buffer está quase cheio, descartamos a
    primeira METADE do conteúdo (as mensagens mais antigas) para liberar
    espaço, em vez de travar ou perder a mensagem nova. Isso é uma estratégia
    simples de "janela deslizante" para logs de tamanho fixo.

    Args:
        text: string com terminador '\0' a ser acrescentada ao log.

    Returns:
        void. Modifica log_chat e log_len diretamente.
*/
static void log_append(const char *text) {
    int n = (int)strlen(text);

    /* Verifica se o texto novo caberia no espaço restante do buffer.
       O "+1" reserva espaço para o '\0' terminador. */
    if (log_len + n + 1 > LOG_MAX) {
        /* Buffer cheio: descartamos a metade mais antiga do log.
           memmove (e não memcpy) é obrigatório aqui porque origem e destino
           se sobrepõem dentro do mesmo array. */
        int metade = LOG_MAX / 2;
        memmove(log_chat, log_chat + metade, log_len - metade);
        log_len -= metade;
    }

    /* Segunda verificação: se o texto for maior que o espaço disponível mesmo
       após o descarte (mensagem gigante), truncamos o que for possível copiar. */
    int copiar = n;
    if (log_len + copiar + 1 > LOG_MAX) copiar = LOG_MAX - log_len - 1;

    if (copiar > 0) {
        memcpy(log_chat + log_len, text, copiar);
        log_len += copiar;
        log_chat[log_len] = '\0'; /* mantém o buffer sempre terminado em \0 */
    }
}


/*
    Verifica e lê mensagens do backend de forma não-bloqueante.

    Esta função é chamada a cada frame do loop de renderização Nuklear.
    Ela é o equivalente Win32 do timer de 100ms que usávamos na IUP
    (IupTimer + callback checar_mensagens), mas integrado diretamente
    ao loop principal em vez de depender de um mecanismo de timer externo.

    A lógica é idêntica à do backend: PeekNamedPipe primeiro, ReadFile
    só se houver dados. Isso garante que a UI NUNCA congela esperando
    por uma mensagem que ainda não chegou.

    Args:
        (sem argumentos — usa os globais g_read e g_write)

    Returns:
        void. Se houver mensagem, ela é adicionada ao log_chat via log_append().
*/
static void poll_backend(void) {
    DWORD disponivel = 0;

    /* Inspeciona o pipe sem bloquear.
       Se PeekNamedPipe falhar (pipe fechado) ou não houver dados, retornamos
       imediatamente sem atrasar o frame de renderização. */
    if (!PeekNamedPipe(g_read, NULL, 0, NULL, &disponivel, NULL) || disponivel == 0)
        return;

    /* Chegamos aqui somente se disponivel > 0: há dados esperando no pipe.
       Podemos chamar ReadFile com segurança — ele não vai bloquear. */
    char buf[512];
    DWORD lidos = 0;
    if (ReadFile(g_read, buf, sizeof(buf) - 1, &lidos, NULL) && lidos > 0) {
        buf[lidos] = '\0';
        log_append(buf); /* injeta a mensagem do backend no log visual */
    }
}


/*
    Procedimento de janela (Window Procedure) — o dispatcher de eventos Win32.

    No Windows, TODA comunicação entre o sistema operacional e a sua janela
    ocorre através desta função de callback. Quando o usuário clica, redimensiona,
    fecha a janela, move o mouse, ou pressiona uma tecla, o Windows empacota
    o evento numa struct MSG e chama esta função com os detalhes.

    No ecossistema Linux/POSIX isso não existe: o GTK, Qt e IUP abstraem
    isso completamente. Aqui estamos um nível abaixo, na API pura.

    A nossa WndProc tem três responsabilidades:
      1. Tratar WM_DESTROY (usuário fechou a janela) → posta WM_QUIT para
         encerrar o loop principal.
      2. Repassar todos os outros eventos ao Nuklear via nk_gdi_handle_event()
         para que ele processe mouse e teclado.
      3. Delegar eventos que nenhum dos dois trata ao comportamento padrão
         do Windows via DefWindowProcW().

    Args:
        hwnd: handle da janela que recebeu o evento.
        msg:  código numérico do evento (ex: WM_DESTROY = 2, WM_KEYDOWN = 256).
        wp:   WPARAM — parâmetro adicional, significado depende do evento.
        lp:   LPARAM — parâmetro adicional, significado depende do evento.

    Returns:
        LRESULT (inteiro longo). Valor de retorno depende do evento tratado.
        Retornar 0 geralmente indica "evento processado, não fazer mais nada".
*/
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* WM_DESTROY é enviado DEPOIS que a janela foi destruída visualmente.
       PostQuitMessage(0) enfileira um WM_QUIT no loop de mensagens,
       o que fará o nosso PeekMessageW retornar WM_QUIT e encerrar o programa. */
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }

    /* nk_gdi_handle_event traduz os eventos Win32 (mouse, teclado, resize)
       para o formato interno do Nuklear. Se ele reconheceu o evento,
       retorna 1 (true) e não precisamos processá-lo mais. */
    if (nk_gdi_handle_event(hwnd, msg, wp, lp)) return 0;

    /* Para tudo que nem nós nem o Nuklear tratamos explicitamente,
       delegamos ao comportamento padrão do Windows (mover janela,
       minimizar, redesenhar borda, etc.). */
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/*
    Inicializa a janela Win32, configura o Nuklear e executa o loop principal da UI.

    Esta é a função mais importante do lado da interface gráfica. Ela substitui
    completamente o fluxo IUP do código original (IupOpen → IupDialog →
    IupMainLoop → IupClose), mas com controle muito mais explícito de cada etapa.

    O loop principal segue este ciclo a cada frame (~16ms = 60 FPS):
      1. Coleta eventos Win32 (mouse, teclado) via PeekMessageW
      2. Repassa esses eventos ao Nuklear (nk_input_begin/end)
      3. Verifica se o backend mandou mensagens (poll_backend)
      4. Descreve o layout da UI com as funções nk_*
      5. Renderiza tudo na tela (nk_gdi_render)
      6. Aguarda 16ms para limitar a ~60 FPS (Sleep)

    Args:
        hInst:   handle da instância do executável (passado pelo WinMain).
                 O Windows usa isso para associar a janela ao nosso processo.
        h_read:  HANDLE do pipe de onde a UI lê mensagens do backend.
        h_write: HANDLE do pipe para onde a UI escreve mensagens do usuário.

    Returns:
        int — sempre 0, indicando encerramento normal.
*/
static int run_ui(HINSTANCE hInst, HANDLE h_read, HANDLE h_write) {
    /* Armazena os handles nos globais para que poll_backend() e o
       handler do botão Enviar possam acessá-los sem parâmetros extras. */
    g_read  = h_read;
    g_write = h_write;

    /* ── REGISTRO DA CLASSE DE JANELA ──────────────────────────────────────
       Antes de criar uma janela no Windows, você precisa registrar uma
       "classe de janela" — um template que define comportamentos comuns:
       qual função de callback vai processar os eventos (lpfnWndProc),
       qual ícone, qual cursor, qual cor de fundo, etc.

       Isso não tem equivalente direto no Linux/IUP: quando você chamava
       IupDialog(), toda essa configuração estava encapsulada internamente.
       Aqui, estamos na camada mais baixa da API do Windows.

       Campos relevantes:
         style         → CS_DBLCLKS permite detectar double-click (usado pelo Nuklear)
         lpfnWndProc   → nossa WndProc acima, que receberá todos os eventos
         hInstance     → associa a classe ao nosso executável
         hCursor       → define o cursor padrão (seta normal)
         lpszClassName → nome da classe, usado em CreateWindowExW abaixo
    */
    WNDCLASSW wc = {0};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"POCChat";
    RegisterClassW(&wc);

    /* ── CRIAÇÃO DA JANELA ─────────────────────────────────────────────────
       AdjustWindowRect ajusta o RECT para compensar a barra de título e
       bordas: se queremos WIN_W x WIN_H de área ÚTIL de conteúdo, a janela
       real precisa ser um pouco maior. Sem isso, a área de conteúdo seria
       menor do que o esperado.

       CreateWindowExW é a função principal de criação de janelas:
         dwExStyle      → 0 = sem estilos estendidos
         lpClassName    → referencia a classe que registramos acima
         lpWindowName   → texto da barra de título
         dwStyle        → WS_OVERLAPPEDWINDOW = janela normal com barra, bordas,
                          botões minimizar/maximizar/fechar
                          WS_VISIBLE = já começa visível (sem ShowWindow() extra)
         x, y           → CW_USEDEFAULT = Windows escolhe a posição inicial
         nWidth, nHeight → tamanho ajustado pelo AdjustWindowRect
    */
    RECT r = {0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, L"POCChat", L"POC Chat \x2014 Win32 + Nuklear GDI",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);

    /* ── INICIALIZAÇÃO DO NUKLEAR ──────────────────────────────────────────
       O Nuklear GDI precisa de:
         1. Uma fonte GDI (nk_gdifont_create): cria um HFONT do Windows com
            a família e tamanho especificados. "Consolas" é monoespaçada,
            ideal para interfaces de chat/terminal.

         2. O Device Context (HDC) da janela: é o "canvas" do GDI. Toda
            operação de desenho no Windows acontece sobre um DC.
            GetDC(hwnd) obtém o DC associado à nossa janela.

         3. nk_gdi_init: inicializa o contexto interno do Nuklear, vinculando
            a fonte e o DC. A partir daqui, ctx é válido e pode ser usado
            para descrever a UI a cada frame.
    */
    HDC dc = GetDC(hwnd);
    font = nk_gdifont_create("Consolas", 14);
    ctx  = nk_gdi_init(font, dc, WIN_W, WIN_H);

    MSG msg_win; /* struct que armazena um evento Win32 desempacotado */

    /* ── LOOP PRINCIPAL ────────────────────────────────────────────────────
       Este é o "coração batendo" da aplicação. Ele substitui o IupMainLoop()
       da versão original, mas de forma explícita e controlada.

       Diferença fundamental entre GetMessage e PeekMessage:
         GetMessage  → BLOQUEIA o processo até chegar um evento. A CPU fica
                       em idle durante esse tempo. Não serve aqui porque
                       precisamos redesenhar a UI e verificar o pipe a cada frame,
                       mesmo quando não há eventos de input.
         PeekMessage → NÃO bloqueia. Retorna imediatamente com TRUE se houver
                       um evento na fila, ou FALSE se a fila estiver vazia.
                       PM_REMOVE remove o evento da fila ao retorná-lo.
                       É o equivalente de um select() não-bloqueante no Linux.
    */
    while (1) {
        /* ── FASE 1: coleta de input ───────────────────────────────────────
           nk_input_begin sinaliza ao Nuklear que vamos alimentá-lo com eventos.
           Todos os eventos coletados dentro deste bloco serão considerados
           parte do mesmo frame. */
        nk_input_begin(ctx);
        while (PeekMessageW(&msg_win, NULL, 0, 0, PM_REMOVE)) {
            /* WM_QUIT é postado por PostQuitMessage() quando a janela fecha.
               Usamos goto para sair do loop while(1) externo de forma limpa,
               indo direto para o bloco de cleanup. */
            if (msg_win.message == WM_QUIT) goto encerrar;

            /* TranslateMessage converte keycodes virtuais (VK_A, VK_SHIFT...)
               em caracteres WM_CHAR — necessário para que campos de texto
               do Nuklear recebam os caracteres corretos.

               DispatchMessageW entrega o evento à WndProc da janela de destino,
               completando o ciclo de processamento do evento. */
            TranslateMessage(&msg_win);
            DispatchMessageW(&msg_win);
        }
        /* Fim da coleta de input para este frame */
        nk_input_end(ctx);

        /* ── FASE 2: IPC não-bloqueante ────────────────────────────────────
           Verifica se o backend enviou alguma mensagem ANTES de renderizar.
           Assim a mensagem aparece no log no mesmo frame em que chegou,
           sem delay visual perceptível. */
        poll_backend();

        /* ── FASE 3: descrição do layout Nuklear (Immediate Mode) ──────────
           nk_begin abre um "painel" Nuklear. Diferente da IUP, que criava
           widgets persistentes com IupText/IupButton, o Nuklear redescreve
           TUDO a cada frame. Se uma função retornar true (ex: nk_button_label),
           significa que o evento ocorreu NESTE frame específico.

           Parâmetros:
             ctx        → o contexto do Nuklear
             "Chat"     → nome interno do painel (deve ser único por sessão)
             nk_rect()  → posição e tamanho: cobre toda a janela (x=0, y=0)
             flags      → NK_WINDOW_NO_SCROLLBAR: desabilita barra de scroll
                          no painel externo (o scroll do log é interno ao edit box)
        */
        if (nk_begin(ctx, "Chat",
                     nk_rect(0, 0, WIN_W, WIN_H),
                     NK_WINDOW_NO_SCROLLBAR)) {

            /* ── LOG DO CHAT ───────────────────────────────────────────────
               nk_layout_row_dynamic: define uma linha de layout com altura
               fixa e 1 coluna que se expande horizontalmente.
               WIN_H - 100 deixa 100px na parte inferior para os controles.

               nk_edit_string_zero_terminated: campo de texto multi-linha
               que opera diretamente sobre o nosso buffer log_chat.
               Flags:
                 NK_EDIT_BOX       → multi-linha com barra de scroll interna
                 NK_EDIT_READ_ONLY → usuário não pode editar (apenas visualizar)
               O último parâmetro (nk_filter_default) seria o filtro de caracteres
               permitidos — irrelevante aqui pois é read-only. */
            nk_layout_row_dynamic(ctx, WIN_H - 100, 1);
            nk_edit_string_zero_terminated(
                ctx,
                NK_EDIT_BOX | NK_EDIT_READ_ONLY,
                log_chat, LOG_MAX,
                nk_filter_default);

            /* ── LINHA DE CONTROLES ────────────────────────────────────────
               nk_layout_row_begin com NK_STATIC: define uma linha onde cada
               widget tem largura fixa em pixels (em vez de proporcional).
               Parâmetros: modo NK_STATIC, altura 32px, 3 colunas.

               Depois usamos nk_layout_row_push() para definir a largura
               de cada coluna antes de chamar o widget correspondente.

               Layout final: [90px ID] [largura restante MSG] [82px Enviar]
            */
            nk_layout_row_begin(ctx, NK_STATIC, 32, 3);

            /* Campo ID do destinatário (90px) */
            nk_layout_row_push(ctx, 90);
            /* nk_edit_string: campo de texto editável de linha única.
               NK_EDIT_SIMPLE = sem multi-linha, sem seleção avançada.
               Lê e escreve diretamente em id_buf; id_len é atualizado
               pelo Nuklear conforme o usuário digita ou apaga. */
            nk_edit_string(ctx, NK_EDIT_SIMPLE,
                           id_buf, &id_len, (int)sizeof(id_buf) - 1,
                           nk_filter_default);

            /* Campo de mensagem (largura = total - campos fixos - margens) */
            nk_layout_row_push(ctx, WIN_W - 90 - 92 - 24);
            nk_edit_string(ctx, NK_EDIT_SIMPLE,
                           msg_buf, &msg_len, (int)sizeof(msg_buf) - 1,
                           nk_filter_default);

            /* Botão Enviar (82px) */
            nk_layout_row_push(ctx, 82);
            /* nk_button_label retorna 1 (true) SOMENTE no frame em que
               o botão foi clicado e solto. Não há callback: a lógica
               fica diretamente no corpo do if(). Isso é o Immediate Mode
               em ação — sem registro de eventos, sem objetos persistentes. */
            if (nk_button_label(ctx, "Enviar")) {
                if (id_len > 0 && msg_len > 0) {
                    /* Garante terminadores: o Nuklear não nul-termina
                       automaticamente os buffers de edit ao final. */
                    id_buf[id_len]   = '\0';
                    msg_buf[msg_len] = '\0';

                    /* Formata o pacote no padrão "destinatário -> mensagem"
                       e envia para o processo backend via WriteFile no pipe.
                       É o equivalente do write(pipe_ui_to_back[1], ...) do Linux. */
                    char pacote[512];
                    _snprintf(pacote, sizeof(pacote),
                              "%s -> %s", id_buf, msg_buf);
                    DWORD wr;
                    WriteFile(g_write, pacote,
                              (DWORD)strlen(pacote), &wr, NULL);

                    /* Exibe imediatamente no log local sem esperar o backend
                       confirmar. Isso dá feedback visual instantâneo ao usuário. */
                    char eco[600];
                    _snprintf(eco, sizeof(eco),
                              "Você (para %s): %s\n", id_buf, msg_buf);
                    log_append(eco);

                    /* Limpa apenas o campo de mensagem após o envio.
                       O campo de ID é mantido para facilitar o envio de
                       múltiplas mensagens para o mesmo destinatário. */
                    memset(msg_buf, 0, sizeof(msg_buf));
                    msg_len = 0;
                }
            }

            nk_layout_row_end(ctx);
        }
        /* nk_end SEMPRE deve ser chamado após nk_begin, mesmo que o bloco
           acima não tenha sido executado. Ele finaliza o painel e empurra
           os comandos de desenho para a fila interna do Nuklear. */
        nk_end(ctx);

        /* ── FASE 4: renderização ──────────────────────────────────────────
           nk_gdi_render processa toda a fila de comandos acumulada pelo
           Nuklear neste frame (retângulos, textos, linhas) e os desenha no
           Device Context via chamadas GDI do Windows.

           O argumento é a cor de fundo (RGB 25, 25, 30 = cinza muito escuro,
           quase preto). Equivale ao "clearColor" em OpenGL. */
        nk_gdi_render(nk_rgb(25, 25, 30));

        /* Limita a ~60 FPS. Sem Sleep, o loop consumiria 100% de CPU
           redesenhando frames desnecessariamente rápido. 16ms ≈ 1/60s. */
        Sleep(16);
    }

encerrar:
    /* ── CLEANUP DA UI ─────────────────────────────────────────────────────
       Libera todos os recursos do Nuklear e do GDI na ordem inversa da
       criação. ReleaseDC devolve o Device Context que obtivemos com GetDC. */
    nk_gdifont_del(font);
    nk_gdi_shutdown();
    ReleaseDC(hwnd, dc);
    return 0;
}


/* =============================================================================
   SEÇÃO 3 — PONTO DE ENTRADA
   =============================================================================
   WinMain é o ponto de entrada de aplicações GUI no Windows, equivalente
   ao main() de aplicações de console. A flag -mwindows do GCC instrui o
   linker a usar WinMain em vez de main() e a não criar a janela de console.

   Parâmetros (equivalência com main):
     hInst     → handle da instância do executável (identifica o .exe na memória)
     hPrev     → sempre NULL em Win32 moderno (era usado no Win16, obsoleto)
     lpCmdLine → linha de comando como string ANSI (preferimos o wide-char abaixo)
     nShow     → como a janela deve aparecer (maximizada, normal, etc.)
                 Não usamos porque passamos WS_VISIBLE direto no CreateWindowExW.
   ============================================================================= */

/*
    Ponto de entrada da aplicação. Decide o modo de execução e orquestra
    a criação dos pipes, do processo filho e da interface gráfica.

    COMO O fork() É SIMULADO NO WINDOWS
    ------------------------------------
    No Linux, fork() clonava o processo atual. O filho herdava os file
    descriptors dos pipes automaticamente (basta que estivessem abertos).

    No Windows não existe clone de processo. A solução é:
      1. Criar os pipes ANTES de qualquer coisa
      2. Marcar as pontas que o filho precisa como herdáveis
      3. Chamar CreateProcess passando o próprio executável com --backend
      4. Serializar os valores numéricos dos HANDLEs na linha de comando
      5. O filho lê esses números de argv e converte de volta para HANDLE

    SOBRE A HERANÇA DE HANDLES
    --------------------------
    CreatePipe com SECURITY_ATTRIBUTES.bInheritHandle = TRUE cria os handles
    já marcados como "herdáveis". Quando CreateProcess é chamado com
    bInheritHandles = TRUE, TODOS os handles herdáveis do processo pai são
    copiados para o processo filho.

    O problema: se não fizermos nada, as QUATRO pontas dos dois pipes seriam
    herdadas. O filho teria handles que não deveria ter, e os pipes nunca
    fechariam corretamente (o OS mantém o pipe aberto enquanto houver qualquer
    handle válido para ele, em qualquer processo).

    A solução é SetHandleInformation: marca as pontas que pertencem APENAS
    ao pai como não-herdáveis ANTES de chamar CreateProcess.

    Args:
        hInst:   handle da instância — repassado para run_ui() criar a janela.
        hPrev:   não usado (Win16 legacy).
        lpCmd:   não usado — usamos GetCommandLineW() para a versão wide-char.
        nShow:   não usado — janela já criada com WS_VISIBLE.

    Returns:
        int — 0 em caso de sucesso, 1 em caso de falha na inicialização.
*/
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    /* CommandLineToArgvW é a forma correta de parsear argv no Windows com
       suporte a Unicode (Wide-char). Retorna um array de ponteiros LPWSTR
       que deve ser liberado com LocalFree() quando não mais necessário.

       Por que não usar lpCmdLine direto? Porque lpCmdLine é ANSI (LPSTR) e
       não suporta caminhos Unicode. GetCommandLineW() retorna a versão
       wide-char completa (incluindo o nome do executável em argv[0]). */
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* ── DETECÇÃO DE MODO: UI ou BACKEND? ─────────────────────────────── */
    if (argc >= 4 && lstrcmpW(argv[1], L"--backend") == 0) {
        /* Estamos rodando como processo FILHO (backend).
           argv[2] e argv[3] contêm os valores numéricos dos HANDLEs
           que o processo pai serializou com _snwprintf + %I64u.

           wcstoull converte wide-string → unsigned long long (64 bits).
           O cast (HANDLE)(UINT_PTR) é o caminho correto para converter
           um inteiro de volta para HANDLE, garantindo que o tamanho do
           ponteiro seja respeitado em ambos 32-bit e 64-bit.

           Ordem dos argumentos (definida quando o pai monta o cmd[]):
             argv[2] = back_to_ui_write → backend ESCREVE aqui (h_write)
             argv[3] = ui_to_back_read  → backend LÊ daqui  (h_read)
        */
        HANDLE h_read  = (HANDLE)(UINT_PTR)wcstoull(argv[2], NULL, 10);
        HANDLE h_write = (HANDLE)(UINT_PTR)wcstoull(argv[3], NULL, 10);
        LocalFree(argv);
        run_backend(h_read, h_write);
        return 0;
    }
    LocalFree(argv);

    /* ── MODO UI: configuração dos pipes IPC ───────────────────────────── */

    /* SECURITY_ATTRIBUTES configura propriedades de segurança e herança.
       Campos:
         nLength              → tamanho da struct (obrigatório)
         lpSecurityDescriptor → NULL = permissões padrão do processo
         bInheritHandle       → TRUE = handles criados com esta struct
                                 serão herdáveis pelo processo filho
    */
    SECURITY_ATTRIBUTES security_atributes = {sizeof(security_atributes), NULL, TRUE};

    /*
     * Criamos DOIS pipes anônimos (equivalente a dois pipe() do Linux):
     *
     *   back_to_ui: backend → UI
     *     back_to_ui_write: o backend escreve respostas aqui
     *     back_to_ui_read:  a UI lê as respostas daqui
     *
     *   ui_to_back: UI → backend
     *     ui_to_back_write: a UI escreve mensagens do usuário aqui
     *     ui_to_back_read:  o backend lê as mensagens daqui
     *
     * No Linux: pipe(fd) retornava fd[0]=leitura, fd[1]=escrita.
     * No Windows: CreatePipe(&hRead, &hWrite, &sa, 0) — mesmo conceito,
     * mas com HANDLEs em vez de file descriptors.
     * O último argumento (0) define o tamanho do buffer: 0 = padrão do sistema.
     */
    HANDLE back_to_ui_read, back_to_ui_write;
    HANDLE ui_to_back_read, ui_to_back_write;

    if (!CreatePipe(&back_to_ui_read, &back_to_ui_write, &security_atributes, 0) ||
        !CreatePipe(&ui_to_back_read, &ui_to_back_write, &security_atributes, 0)) {
        MessageBoxW(NULL, L"Falha ao criar pipes IPC.", L"Erro", MB_ICONERROR);
        return 1;
    }

    /* ── CONTROLE DE HERANÇA DOS HANDLES ───────────────────────────────────
       Problema: todos os 4 handles foram criados com bInheritHandle = TRUE.
       Se chamarmos CreateProcess agora, o filho herdaria os 4 — incluindo
       as pontas que pertencem APENAS ao processo pai.

       Isso causaria um bug silencioso: quando o pai fecha suas pontas,
       o OS não destruiria o pipe porque o filho ainda teria handles para
       as mesmas pontas. O pipe ficaria "pendurado" para sempre.

       Solução: SetHandleInformation remove o flag HANDLE_FLAG_INHERIT
       das pontas que o pai usa (e o filho NÃO deve herdar).

       Pontas do PAI (UI):
         back_to_ui_read   → pai LÊ respostas do backend: NÃO herdar
         ui_to_back_write  → pai ESCREVE mensagens do usuário: NÃO herdar

       Pontas do FILHO (backend) — permanecem herdáveis:
         back_to_ui_write  → filho ESCREVE respostas: herdar 
         ui_to_back_read   → filho LÊ mensagens: herdar 
    */
    SetHandleInformation(back_to_ui_read,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ui_to_back_write, HANDLE_FLAG_INHERIT, 0);

    /* ── LANÇAMENTO DO PROCESSO BACKEND ────────────────────────────────────
       GetModuleFileNameW obtém o caminho completo do próprio executável.
       No Linux, o equivalente seria ler /proc/self/exe.
       Usamos isso para que CreateProcess relance exatamente o mesmo binário. */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    /* Monta a linha de comando para o processo filho.
       Formato: "<caminho/poc_chat.exe>" --backend <back_to_ui_write> <ui_to_back_read>

       %I64u é o especificador Windows para unsigned __int64 (equivale ao %llu
       do padrão C99). Fazemos o cast do HANDLE para inteiro antes de formatar.
       O filho vai reverter esse processo com wcstoull() em argv[2] e argv[3].

       As aspas ao redor de "%s" (o caminho do exe) são essenciais: caminhos
       do Windows frequentemente contêm espaços (ex: "C:\Program Files\...").
       Sem as aspas, o CreateProcess interpretaria o espaço como separador
       de argumento e falharia ao tentar encontrar o executável. */
    wchar_t cmd[MAX_PATH + 128];
    _snwprintf(cmd, _countof(cmd),
               L"\"%s\" --backend %I64u %I64u",
               exe,
               (unsigned __int64)(UINT_PTR)back_to_ui_write,
               (unsigned __int64)(UINT_PTR)ui_to_back_read);

    /* STARTUPINFOW descreve como a janela (se houver) do processo filho
       deve ser criada. Inicializamos zerado e só definimos nLength.
       Como o backend não tem janela (-mwindows), a maioria dos campos
       não importa.

       PROCESS_INFORMATION receberá, após CreateProcessW, os handles e IDs
       do processo filho criado — necessários para TerminateProcess e
       WaitForSingleObject no cleanup. */
    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};

    /* CreateProcessW — o substituto do fork()+exec() do Linux.
       Parâmetros relevantes:
         lpApplicationName  → NULL: o nome do exe está incluído em lpCommandLine
         lpCommandLine      → cmd[] montado acima (modificável, daí não ser const)
         lpProcessAttributes, lpThreadAttributes → NULL = segurança padrão
         bInheritHandles    → TRUE: o filho herda os handles herdáveis do pai
                              (os dois que deixamos com HANDLE_FLAG_INHERIT ativo)
         dwCreationFlags    → 0: sem flags especiais (sem console separado, etc.)
         lpEnvironment      → NULL: herda as variáveis de ambiente do pai
         lpCurrentDirectory → NULL: herda o diretório atual do pai
    */
    if (!CreateProcessW(NULL, cmd, NULL, NULL,
                        TRUE,  /* bInheritHandles */
                        0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"Falha ao iniciar processo backend.", L"Erro", MB_ICONERROR);
        return 1;
    }

    /* ── FECHA AS PONTAS DO FILHO NO PROCESSO PAI ──────────────────────────
       Agora que o filho foi criado e herdou suas pontas, o pai deve fechar
       as cópias que ficaram no seu próprio handle table.

       Por que isso é obrigatório?
       O OS mantém o pipe vivo enquanto existir QUALQUER handle aberto para ele.
       Se o pai mantiver back_to_ui_write aberto, mesmo quando o backend morrer
       e fechar sua cópia, o pipe "back_to_ui" nunca sinaliza EOF para o pai.
       A UI ficaria esperando leituras de um pipe que nunca vai fechar.

       No Linux: close(pipe_back_to_ui[1]) no processo pai — exatamente o mesmo conceito. */
    CloseHandle(back_to_ui_write);
    CloseHandle(ui_to_back_read);

    /* ── EXECUTA A INTERFACE GRÁFICA ───────────────────────────────────────
       run_ui bloqueia até o usuário fechar a janela. */
    int result = run_ui(hInst, back_to_ui_read, ui_to_back_write);

    /* ── CLEANUP FINAL ─────────────────────────────────────────────────────
       TerminateProcess força o encerramento do backend (equivale a kill(pid, SIGKILL)).
       WaitForSingleObject aguarda o processo filho terminar de fato antes de
       liberar os handles — equivale a waitpid(pid, NULL, 0).
       3000ms = timeout máximo de espera antes de prosseguir de qualquer forma. */
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(back_to_ui_read);
    CloseHandle(ui_to_back_write);

    return result;
}
