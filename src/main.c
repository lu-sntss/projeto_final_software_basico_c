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

int pipe_fd[2]; // fd[0] é para ler, fd[1] é para escrever

/*
    Método Daemon com loop de 100ms

    Checa o pipe para verificar se existe mensagem.
*/
int checar_mensagens(Ihandle *ih){
    char buffer[256];
    
    // Tenta ler o pipe.
    // Caso não exista mensagem, ignora 
    int byte_lidos = read(pipe_fd[0], buffer, sizeof(buffer) -1 );
    
    if (byte_lidos > 0){
        buffer[byte_lidos] = '\0'; // Garante o fim da string;

        // Injeta o texto na tela
        IupSetAttribute(txt_chat, "APPEND", buffer);
    }
    return IUP_DEFAULT; // Mantém o timer rodando
}

void rodar_backend(){
    close(pipe_fd[0]); // O filho não lê, apenas escreve
    int contador = 1;
    char msg[256];

    while (1){
        sleep(3); // Evita overhead do processo!!!!

        sprintf(msg, "[Backend PID %d] Mensagem %d chegou via Pipe!\n", getpid(), contador++);

        // EMpurra a mensagem pelo pipe
        write(pipe_fd[1], msg, strlen(msg));
    }
}

int main(int argc, char **argv){
    // Cria o Pipe de comunicação antes de clonar
    if (pipe(pipe_fd) == -1){
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
        close(pipe_fd[1]); // Apenas escuta

        // Transforma a leitura em operação não bloqueante
        int flags = fcntl(pipe_fd[0], F_GETFL, 0);
        fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

        // Inicializa a interface gráfica (IUP)
        IupOpen(&argc, &argv);

        // Monta os visuais
        txt_chat = IupText(NULL);
        IupSetAttribute(txt_chat, "MULTILINE", "YES");
        IupSetAttribute(txt_chat, "EXPAND", "YES");
        // Names até onde sei, a documentação é terrivel
        // WRITE - ESCREVER
        // READONLY - Esse componente serve somente como leitura
        IupSetAttribute(txt_chat, "WRITE", "YES");

        Ihandle *btn_teste = IupButton("Testar Interface", NULL);
        Ihandle *vbox = IupVbox(txt_chat, btn_teste, NULL);
        Ihandle *janela = IupDialog(vbox);

        IupSetAttribute(vbox, "ALIGNMENT", "ARIGHT");
        IupSetAttribute(vbox, "GAP", "10");
        IupSetAttribute(vbox, "MARGIN", "10x10");

        // Mostra o PID do pai no titulo
        char titulo[100];
        sprintf(titulo,"POC Chat (IUP PID: %d)", getpid());
        IupSetAttribute(janela, "TITLE",titulo);
        IupSetAttribute(janela, "SIZE","250x150"); // Tamanho da janela

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