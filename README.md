# projeto_final_software_basico_c

Repositório para implementação e deploy de um projeto com o objetivo de validar meus conhecimentos em estrutura de dados e programação de software básico em C puro.

---

## Prova de Conceito (POC): Mensageria ao vivo utilizando arquitetura de multiprocessamento

### 1. Justificativa 

Essa é uma prova de conceito que desenvolvi para um sistema backend de mensageria ao vivo, validando também a biblioteca IUP para interface gráfica nativa em C.

**1.1. Bibliotecas para mensageria e paralelismo**
Para o recebimento e envio de mensagens, proponho o uso de um sistema de multiprocessamento. Essa abordagem é significativamente mais segura que o uso de multithreading interno em C, já que me permite criar uma arquitetura mais próxima de microsserviços em troca de um custo de hardware maior, porém efêmero.
As bibliotecas que utilizei na integração foram:
*   `unistd.h` (método `fork()`): Para a ruptura dos processos e criação do paralelismo nativo.
*   `fcntl.h`: Para manipulação e comunicação entre os *pipes* dos processos e atribuição da flag de leitura não-bloqueante (`O_NONBLOCK`), evitando a interrupção do programa durante as rotinas de checagem e envio de mensagens.

**1.2. Interface Gráfica**
Para a interface, estudei e apliquei a lib IUP (Portable User Interface), criada pela PUC-Rio. Ela propõe ser uma interface gráfica leve e de simples implementação, escrita inteiramente em C.
Apesar da minha implementação ter sido um sucesso, notei que a documentação da biblioteca é de difícil compreensão. Esse atrito de aprendizado é um ponto de atenção que considero para a adoção da biblioteca no meu sistema final.

**1.3. Banco de Dados**
Embora eu não tenha incluído o banco de dados nesta POC, já adianto que a integração poderá ser feita em SQLite. É uma solução extremamente mais simples, de fácil instalação, muito leve na execução e que cumpre todos os requisitos do projeto. Apresenta como contrapartida uma segurança mais baixa, o que considerei perfeitamente aceitável dentro do escopo acadêmico deste projeto.

> **Nota:** Os binários dinâmicos da IUP para Linux já deixei devidamente dispostos no diretório `libs/` do projeto.

---

### 2. Instalação de Dependências

A biblioteca IUP atua sobre o subsistema X11/GTK2 no Linux. Portanto, para testar a aplicação, instale as dependências de desenvolvimento do GTK2:

bash
sudo apt update
sudo apt install libgtk2.0-dev

3. Procedimento de Compilação

A partir do diretório raiz do projeto, acione o compilador GCC determinando as rotas para inclusão de cabeçalhos (-I) e linkagem das bibliotecas dinâmicas (-L) da IUP e GTK2 nativas que configurei:
    Bash
    
    gcc src/main.c -o poc_pipe -I./libs/iup-3.9_Linux32_64_lib/include -L./libs/iup-3.9_Linux32_64_lib -liup -lgtk-x11-2.0 -lgdk-x11-2.0

Nota: Desenvolvi e testei a POC inteiramente no ambiente Linux (Ubuntu / distribuições baseadas em Debian). Para execução em Windows, será necessária a etapa de instalação da lib e configurações de compilação equivalentes para a plataforma.

4. Procedimento de Execução

    Tendo em vista que deixei as dependências dinâmicas da IUP contidas em um diretório local em vez dos repositórios globais do sistema, é estritamente necessário exportar o caminho das bibliotecas para a variável de ambiente antes ,e SEMPRE QUE RESETAR O TERMINAL, do disparo do binário final:
        Bash
        
        export LD_LIBRARY_PATH=./libs/iup-3.9_Linux32_64_lib:$LD_LIBRARY_PATH
        ./poc_pipe

5. Resultados Obtidos

A execução da minha Prova de Conceito registrou êxito absoluto, ratificando a arquitetura que propus através dos seguintes marcos:

    Comprovação do Isolamento: As métricas capturadas em tela revelaram PIDs (Identificadores de Processo) distintos para o componente gráfico e o serviço de rede de background, atestando o funcionamento em instâncias físicas de memória particionadas.

    Segurança em Testes de Estresse: A Interface Gráfica de Usuário (GUI) manteve-se integralmente responsiva ao ser submetida a sobrecargas artificiais de eventos no sistema operacional. Não registrei nenhum congelamento temporário ou Segmentation Fault.

    Gerenciamento Estrutural do Fluxo: O dimensionamento de delays que estruturei em conjunto com a leitura não-bloqueante mitigou qualquer risco de transbordamento do buffer local de comunicação (limite de 64KB do Kernel Linux), imunizando a aplicação contra anomalias internas no pipe.

Abaixo temos uma imagem do processo e seus sub processos sendo executados em paralelo.

pid 9188 -> verificação de mensagens.
pid 9190 -> envio de mensagens.

Apesar que eu não sei se na arquitetura final o envio de mensagens deva ser um processo filho a checagem

![Processos rodando em paralelo](images/readme/image.png)
