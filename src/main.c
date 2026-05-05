/*
sudo apt update
sudo apt install libgtk2.0-dev

gcc src/main.c -o poc_pipe -I./libs/iup-3.9_Linux32_64_lib/include -L./libs/iup-3.9_Linux32_64_lib -liup -lgtk-x11-2.0 -lgdk-x11-2.0
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>      // Para manipular os arquivos/tubos
#include <sys/wait.h>   // Para lidar com processos filhos
#include <iup.h>        // Nossa interface gráfica

// Ponteiro global para a caixa de texto
Ihandle *txt_chat;
Ihandle *txt_id; // Ponteiro para o id do destinatário
Ihandle *txt_msg; // Ponteiro para mensagem que será enviada

//fd[0] é para ler, fd[1] é para escrever
int pipe_back_to_ui[2]; 
//fd[0] é para ler, fd[1] é para escrever
int pipe_ui_to_back[2];

/*
    Callback do botão Enviar
    Puxa os dados de id e mensagem para injetar no backend
*/
int enviar_msg(Ihandle *ih){
    // Basicamente um get HTML do dado de um formulário
    char *id_str = IupGetAttribute(txt_id, "VALUE");
    char *msg_str = IupGetAttribute(txt_msg, "VALUE");
    
    if (id_str && msg_str && strlen(id_str) >0 ){
        char pacote[512];

        // Formata a mensagem com um separador ("5001 -> Olá mundo")
        sprintf(pacote, "%s -> %s", id_str, msg_str);

        // Envia mensagem para o backend usando pipe de escrita
        write(pipe_ui_to_back[1], pacote, strlen(pacote));

        // Escreve na propria tela oq o usuário enviou
        char log_local[600];
        sprintf(log_local, "Você (para %s): %s\n",id_str, msg_str);
        IupSetAttribute(txt_chat, "APPEND", log_local);
    }
    return IUP_DEFAULT;
}

/*
    Método Daemon com loop de 100ms

    Checa o pipe para verificar se existe mensagem.
*/
int checar_mensagens(Ihandle *ih){
    char buffer[256];
    
    // Tenta ler o pipe.
    // Caso não exista mensagem, ignora 
    int byte_lidos = read(pipe_back_to_ui[0], buffer, sizeof(buffer) -1 );
    
    if (byte_lidos > 0){
        buffer[byte_lidos] = '\0'; // Garante o fim da string;

        // Injeta o texto na tela
        IupSetAttribute(txt_chat, "APPEND", buffer);
    }
    return IUP_DEFAULT; // Mantém o timer rodando
}

void rodar_backend(){
    // Fecha as pontas dos canos que o filho não usa
    close(pipe_back_to_ui[0]); 
    close(pipe_ui_to_back[1]); 

    // Leitura de dados do pipe sem interrupção do sistema geral.
    // Clasuça de não interr
    int flags = fcntl(pipe_ui_to_back[0], F_GETFL, 0);
    fcntl(pipe_ui_to_back[0], F_SETFL, flags | O_NONBLOCK);

    char buffer_entrada[512];
    char log_backend[600];

    while (1){
        usleep(100000); // Evita overhead do processo!!!!

        // Loop de verficação da mensagem
        int lidos = read(pipe_ui_to_back[0], buffer_entrada, sizeof(buffer_entrada) - 1);
        if (lidos >0){
            buffer_entrada[lidos] = '\0';

            //
            sprintf(log_backend, "[Sistema] Backend preparando para rotear pacote: %s\n", buffer_entrada);
            write(pipe_back_to_ui[1], log_backend, strlen(log_backend));
        }

        // Aqui pra baixo vamos preparar a escuta em rede usando Sockets

    }
}

int main(int argc, char **argv){
    // Cria o Pipe de comunicação antes de clonar
    if (pipe(pipe_back_to_ui) == -1 || pipe(pipe_ui_to_back) == -1){
        perror("Erro ao criar pipe");
        return 1; // Finaliza
    }

    // RUPTURA!!
    pid_t pid = fork();
    if (pid <0){
        perror("Erro no fork");
        return 1;
    }

    if (pid == 0){
        // Processo do backend
        rodar_backend();
        exit(0);
    } else {
        // Processo da interface gráfica
        close(pipe_back_to_ui[1]); // Apenas escuta
        close(pipe_ui_to_back[0]); // Apenas escrita

        // Transforma a leitura em operação não bloqueante
        int flags = fcntl(pipe_back_to_ui[0], F_GETFL, 0);
        fcntl(pipe_back_to_ui[0], F_SETFL, flags | O_NONBLOCK);

        // Inicializa a interface gráfica (IUP)
        IupOpen(&argc, &argv);

        // Monta os visuais
        txt_chat = IupText(NULL);
        IupSetAttribute(txt_chat, "MULTILINE", "YES");
        IupSetAttribute(txt_chat, "EXPAND", "YES");
        // Names até onde sei, a documentação é terrivel
        // WRITE - ESCREVER
        // READONLY - Esse componente serve somente como leitura
        IupSetAttribute(txt_chat, "READONLY", "YES");

        // Cria os campos de entrada
        txt_id = IupText(NULL);
        IupSetAttribute(txt_id, "SIZE", "40x"); // Largura fixa pro ID
        IupSetAttribute(txt_id, "CUEBANNER", "ID"); // Placeholder (Texto fantasma)

        txt_msg = IupText(NULL);
        IupSetAttribute(txt_msg, "EXPAND", "HORIZONTAL");
        IupSetAttribute(txt_msg, "CUEBANNER", "Digite sua mensagem...");

        Ihandle *btn_enviar = IupButton("Enviar", NULL);
        IupSetCallback(btn_enviar, "ACTION", (Icallback)enviar_msg); // Conecta o clique

        // Agrupa os controles de baixo numa linha horizontal (Hbox)
        Ihandle *linha_controles = IupHbox(txt_id, txt_msg, btn_enviar, NULL);
        IupSetAttribute(linha_controles, "GAP", "5"); // Espaço entre eles
        IupSetAttribute(linha_controles, "MARGIN", "5x5");

        // Agrupa o chat e a linha de controles numa coluna vertical (Vbox)
        Ihandle *vbox = IupVbox(txt_chat, linha_controles, NULL);
        IupSetAttribute(vbox, "MARGIN", "5x5");

        Ihandle *janela = IupDialog(vbox);

        // Mostra o PID do pai no titulo
        char titulo[100];
        sprintf(titulo,"POC Chat - Mensageiro (IUP PID: %d)", getpid());
        IupSetAttribute(janela, "TITLE",titulo);
        IupSetAttribute(janela, "SIZE","300x200"); // Tamanho da janela

        // Cria o timer que executa a função de checar mensagem
        Ihandle *timer = IupTimer();
        IupSetInt(timer, "TIME", 100);
        IupSetCallback(timer, "ACTION_CB", (Icallback)checar_mensagens);
        IupSetAttribute(timer, "RUN", "YES");

        // Roda a interface gráfica
        // Elemento para redenrizar, posição x, posição y
        // Porra de lib mal documentada do caralho
        IupShowXY(janela, IUP_CENTER, IUP_CENTER);
        IupMainLoop();

        IupClose();
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return 0; // Encerra loop;
}