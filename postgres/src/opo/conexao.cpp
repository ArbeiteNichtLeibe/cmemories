// conexao.cpp
// ============================================================
// GERENCIADOR DE CONEXÕES POSTGRESQL (C++ PURO)
// ============================================================
Você tem toda razão. Essa é a diferença entre um código que "funciona" e um sistema pronto para produção. O que você está descrevendo é a transição de um modelo de **Polling (sondagem)** para um modelo de **Eventos (notificação)**.

Atualmente, se você quiser saber se há dados, sua aplicação precisa chamar `postgresql_listener_bytes_ready` repetidamente para todos os slots. Isso é ineficiente e consome ciclos de CPU desnecessários.

Aqui estão sugestões de como implementar esses "peças que faltam":

---

### 1. O Sistema de "Ticket" (Notificação de Eventos)

Para evitar que a sua aplicação principal fique perguntando "tem dados?" o tempo todo, você pode implementar um sistema de **fila de notificação (Producer-Consumer)**.

* **A Ideia:** Toda vez que um `receiverLoop` recebe dados e os coloca no buffer, ele "emite um ticket" (adiciona o ID do listener em uma fila segura).
* **Implementação sugerida:**
* Adicione uma `std::queue<int> m_notificationQueue` na classe `PostgreSQLService`.
* Proteja essa fila com um `std::mutex` e use uma `std::condition_variable`.
* No método `appendToBuffer`, após copiar os dados para o ring buffer, você faz um `push` do `id` do listener na fila e notifica (`notify_one`) a thread principal.


* **Vantagem:** A sua thread principal pode ficar dormindo em um `wait()` e acordar exatamente quando um "ticket" chega, sabendo exatamente qual slot tem dados novos, sem ter que verificar os 10 buffers.

---

### 2. "Beggin..." (Transaction Management)

Assumindo que por "beggin..." você se refere ao controle de transações (`BEGIN`, `COMMIT`, `ROLLBACK`), esse é um ponto crítico em sistemas que usam `senderExecute`.

* **O Problema:** O `senderExecute` é stateless. Se você enviar um `BEGIN` e, na próxima chamada, o sistema conectar em outra sessão ou o `sender` cair e reconectar, você perde o contexto da transação.
* **A Solução:**
* **Manter a Conexão:** Para transações, você precisa garantir que o comando `BEGIN` e o `COMMIT` rodem na **mesma sessão** (`PGconn`).
* **Transaction Wrapper:** Crie um método ou classe `TransactionScope` que garanta que, uma vez aberto um `BEGIN`, as operações subsequentes sejam enviadas para o mesmo descritor de conexão até o `COMMIT`.
* Se for um sistema de alta carga, talvez valha a pena dedicar um "Slot" de listener especificamente para transações longas, ou criar um método `senderExecuteTransaction(const char* sql, bool isBegin, bool isCommit)`.



---

### 3. "Outras Coisas Interessantes" (Melhorias de Resiliência)

Já que você está construindo uma camada de abstração para o PostgreSQL, considere adicionar estes itens para tornar o sistema "a prova de balas":

* **Heartbeat / Health Check:** Atualmente, o `receiverLoop` detecta falhas quando o `poll` ou `PQconsumeInput` falha. Adicione um mecanismo onde a thread principal envia um "PING" (ex: `SELECT 1`) periodicamente para os listeners que estão inativos há muito tempo, para garantir que a conexão não foi cortada silenciosamente por um firewall ou load balancer.
* **Load Balancing de Leitores:** Se você tiver vários listeners, em vez de sempre usar o Listener 0, crie uma lógica simples de **Round Robin** ou **Least-Busy** (escolher o buffer que tem menos bytes prontos) para distribuir o trabalho de processamento de mensagens.
* **Circuit Breaker (Disjuntor):** Se o banco de dados cair, o `reconectarPG` vai tentar reconectar freneticamente. Adicione uma lógica de "backoff exponencial". Se falhar 5 vezes, espere 1 segundo; se falhar mais, espere 5, depois 30. Isso evita que o seu serviço sature o log do banco de dados (e o seu próprio) durante uma queda.
* **Monitoramento de Buffer:** Adicione um método para verificar a "saúde" do buffer. Se o buffer estiver sempre 90% cheio, o sistema está perdendo performance. Um log de aviso (`WARN`) quando o buffer atinge um limite crítico seria muito útil para identificar gargalos na sua lógica de consumo.

---

**Resumo das prioridades:**

1. **Ticket System:** Prioridade máxima se você quiser reduzir o uso de CPU.
2. **Transaction Management:** Fundamental se o sistema fizer escrita de dados.
3. **Circuit Breaker:** Essencial para estabilidade em produção.

Qual desses itens parece ser o gargalo ou a necessidade mais urgente para a sua aplicação hoje?
#include "conexao.hpp"
#include "socketraw.hpp"
#include <cstring>
#include <cstdio>

// ============================================================
// CONSTRUTOR / DESTRUTOR
// ============================================================

GerenciadorConexao::GerenciadorConexao() 
    : m_service(nullptr)
    , m_initialized(false) {
}

GerenciadorConexao::~GerenciadorConexao() {
    fecharTodasConexoes();
}

// ============================================================
// ABRIR CONEXÃO
// ============================================================

int GerenciadorConexao::abrirConexao(const ConexaoConfig& config, char* outError, size_t errSize) {
    return socketraw_bringUp(config.pgSocketPath, config.clientPort, outError, errSize);
}

// ============================================================
// FECHAR CONEXÃO
// ============================================================

bool GerenciadorConexao::fecharConexao(int connectionId, char* outError, size_t errSize) {
    return socketraw_bringDown(connectionId, outError, errSize);
}

void GerenciadorConexao::fecharTodasConexoes() {
    socketraw_bringDownAll();
}

// ============================================================
// PING
// ============================================================

bool GerenciadorConexao::ping(int connectionId) {
    return socketraw_isAlive(connectionId);
}

// ============================================================
// CONSULTAS
// ============================================================

int GerenciadorConexao::getConexoesAtivas() const {
    return socketraw_getActiveCount();
}

// ============================================================
// SINGLETON DO GERENCIADOR
// ============================================================

static GerenciadorConexao& getGerenciador() {
    static GerenciadorConexao instance;
    return instance;
}

// ============================================================
// FUNÇÕES C-STYLE
// ============================================================

int conexao_abrir(const char* pgSocketPath, int clientPort, char* outError, size_t errSize) {
    if (!pgSocketPath) {
        if (outError) snprintf(outError, errSize, "pgSocketPath nulo");
        return -1;
    }
    
    ConexaoConfig config;
    strncpy(config.pgSocketPath, pgSocketPath, MAX_PATH_LEN - 1);
    config.pgSocketPath[MAX_PATH_LEN - 1] = '\0';
    config.clientPort = clientPort;
    config.keepaliveSeconds = 30;
    config.reconnectDelayMs = 5000;
    
    return getGerenciador().abrirConexao(config, outError, errSize);
}

int conexao_fechar(int connectionId, char* outError, size_t errSize) {
    return getGerenciador().fecharConexao(connectionId, outError, errSize) ? 0 : -1;
}

void conexao_fechar_todas() {
    getGerenciador().fecharTodasConexoes();
}

int conexao_ping(int connectionId) {
    return getGerenciador().ping(connectionId) ? 1 : 0;
}

int conexao_get_active_count() {
    return getGerenciador().getConexoesAtivas();
}