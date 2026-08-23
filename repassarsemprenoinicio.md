### 📜 Normativas do Projeto (Diretrizes Absolutas)

> **Instrução para a IA:** Estas são as regras definitivas do projeto. Leia-as antes de propor qualquer alteração. Não presuma padrões externos que as contrariem.

#### 🖥️ Ambiente e Compilação

* **Ambiente:** Debian 13 ("Trixie") com processador AMD Ryzen 7 e 48 GB RAM.
* **C++ Moderno:** Uso exclusivo de C++23 via GCC 14.2+.
* **Flags Obrigatórias:** `-std=c++23 -Wall -Wextra -Wpedantic -O3 -march=native -pthread -fPIC -MMD -MP`.

#### 🏗️ Arquitetura e Estrutura

* **Autocontenção:** Cada arquivo `.cpp` resolve suas próprias questões. Não faça *forward declarations*; inclua os headers completos ou declare no local de uso.
* **Isolamento de Escopo ("Sem Fofoca"):** É proibido o acesso direto a membros privados ou variáveis globais. Use parâmetros explícitos, getters ou injeção de dependência.
* **Implementação Real:** Sem *mocks* em produção. Se uma função é necessária, implemente-a de verdade.

#### 💾 Gerenciamento de Memória e Recursos (Foco Máximo)

* **Prioridade Absoluta à Arena:** Evite o uso do heap ou da stack padrão para prevenir fragmentação. O uso do `MemoryManagerV2` (arena allocator) é a regra principal.
* **Uso Restrito da Standard Library:** Tenha extremo cuidado com `std::string`, `std::vector`, `std::filesystem`, etc., pois eles dependem de alocação no heap por padrão.
* **RAII Customizado:** O padrão RAII é obrigatório para arquivos e locks, mas para memória, ele deve ser adaptado para funcionar com o `MemoryManagerV2` em vez do heap.
* **Tipos e Inicialização:** Proibido usar `std::memset` ou `std::memcpy` em tipos não-POD. Inicialize tudo com `{}` ou construtores.
* **Proibições de C-Style:** Evite `void*`, casts no estilo C e é **expressamente proibido** o uso de funções `extern "C"`.

#### ⚠️ Tratamento de Erros e Validações

* **Nenhuma Exceção:** Por padrão, o uso de exceções (`try/catch` ou `std::expected`) é proibido, a menos que autorizado pelo revisor.
* **Retorno Padrão:** Toda função que pode falhar deve retornar um `bool` de sucesso e popular um parâmetro de saída com a mensagem (ex: `char* outError, size_t errSize`).
* **Validação Imediata:** Verifique retornos do sistema (como `errno`) imediatamente após a chamada.

#### 🔄 Concorrência

* **Variáveis Compartilhadas:** Devem ser estritamente protegidas por `std::atomic` ou mutex.
* **Threads:** Evite *busy-wait* usando `std::condition_variable`. Como o projeto já usa `pthread`, a migração para `std::thread` ou `std::jthread` (C++20/23) é permitida com cautela.
*  Nomenclatura... as variaveis e funções precisam ser grafadas sempre em lingua inglesa (padrão americano)


### 
Ollama ser usado para embeddings ==> ollama run qwen3-embedding:8b "Funciona aqui??"
e para consultas ==>  ollama run qwen3.6:27b 

### se precisar usar prgama once ou quqlquer tipo de forward declarations, somente mediante autorização!! Peça e justifique!! E não use.!!
