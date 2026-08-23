#!/bin/bash

## Iniciar serviços
#~/llama_ctl.sh start

# Ver status
#~/llama_ctl.sh status

# Ver logs em tempo real
#~/llama_ctl.sh logs

# Monitorar em tempo real (atualiza a cada 3s)
#~/llama_ctl.sh monitor

# Parar serviços
#~/llama_ctl.sh stop

# Reiniciar serviços
#~/llama_ctl.sh restart

# Ajuda
#~/llama_ctl.sh

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configurações
LLAMA_BIN="/home/andre/compilar/llamacpp/build/bin/llama-server"
MODEL_DIR="/home/andre/compilar/guffs"
MODEL_CHAT="$MODEL_DIR/Meta-Llama-3-8B-Instruct-Q4_K_M.gguf"
MODEL_EMBED="$MODEL_DIR/nomic-embed-text-v1.5.f32.gguf"
PORT_CHAT=8080
PORT_EMBED=8081
LOG_DIR="/home/andre/logs"
PID_FILE="/tmp/llama.pids"

# Função para mostrar ajuda
show_help() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Llama.cpp Controller${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    echo -e "${YELLOW}Uso:${NC} $0 {start|stop|restart|status|logs|monitor}"
    echo ""
    echo -e "${YELLOW}Comandos:${NC}"
    echo -e "  ${GREEN}start${NC}     - Inicia os serviços"
    echo -e "  ${GREEN}stop${NC}      - Para os serviços"
    echo -e "  ${GREEN}restart${NC}   - Reinicia os serviços"
    echo -e "  ${GREEN}status${NC}    - Mostra status dos serviços"
    echo -e "  ${GREEN}logs${NC}      - Mostra logs em tempo real"
    echo -e "  ${GREEN}monitor${NC}   - Monitora GPUs e serviços"
    echo ""
}

# Função para verificar se o serviço está rodando
is_running() {
    local port=$1
    if lsof -i :$port > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

# Função para obter PID
get_pid() {
    local port=$1
    lsof -ti :$port 2>/dev/null
}

# Função para iniciar serviços
start_services() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Iniciando Llama.cpp Services${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # Verifica se já está rodando
    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${YELLOW}⚠️  Serviços já estão rodando!${NC}"
        echo -e "Use 'stop' primeiro ou 'restart' para reiniciar."
        return 1
    fi

    # Cria diretório de logs
    mkdir -p $LOG_DIR

    # Verifica executável
    if [ ! -f "$LLAMA_BIN" ]; then
        echo -e "${RED}ERRO: Executável não encontrado: $LLAMA_BIN${NC}"
        return 1
    fi

    # Verifica modelos
    if [ ! -f "$MODEL_CHAT" ]; then
        echo -e "${RED}ERRO: Modelo de chat não encontrado: $MODEL_CHAT${NC}"
        return 1
    fi
    if [ ! -f "$MODEL_EMBED" ]; then
        echo -e "${RED}ERRO: Modelo de embed não encontrado: $MODEL_EMBED${NC}"
        return 1
    fi

    echo -e "${GREEN}✓ Modelos encontrados${NC}"
    echo ""

    # Inicia servidor de chat
    echo -e "${YELLOW}[1/2] Iniciando servidor de CHAT (porta $PORT_CHAT)...${NC}"
    $LLAMA_BIN \
        -m "$MODEL_CHAT" \
        -ngl 999 \
        -ts 2,1 \
        --host 0.0.0.0 \
        --port $PORT_CHAT \
        --cache-type-k q8_0 \
        --cache-type-v q8_0 \
        --flash-attn auto \
        -b 512 \
        -ub 512 \
        --threads 8 \
        --threads-batch 4 \
        > $LOG_DIR/llama-chat.log 2>&1 &

    CHAT_PID=$!
    echo -e "${GREEN}✓ Chat iniciado (PID: $CHAT_PID)${NC}"

    # Aguarda chat iniciar
    sleep 5
    if ! is_running $PORT_CHAT; then
        echo -e "${RED}ERRO: Chat falhou ao iniciar${NC}"
        tail -20 $LOG_DIR/llama-chat.log
        return 1
    fi

    # Inicia servidor de embeddings
    echo -e "${YELLOW}[2/2] Iniciando servidor de EMBEDDINGS (porta $PORT_EMBED)...${NC}"
    $LLAMA_BIN \
        -m "$MODEL_EMBED" \
        --host 0.0.0.0 \
        --port $PORT_EMBED \
        --pooling cls \
        -ngl 999 \
        -ts 2,1 \
        -b 512 \
        -ub 512 \
        --threads 8 \
        --threads-batch 4 \
        > $LOG_DIR/llama-embed.log 2>&1 &

    EMBED_PID=$!
    echo -e "${GREEN}✓ Embeddings iniciado (PID: $EMBED_PID)${NC}"

    # Aguarda embeddings iniciar
    sleep 5
    if ! is_running $PORT_EMBED; then
        echo -e "${RED}ERRO: Embeddings falhou ao iniciar${NC}"
        tail -20 $LOG_DIR/llama-embed.log
        return 1
    fi

    # Salva PIDs
    echo "$CHAT_PID $EMBED_PID" > $PID_FILE

    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}✅ SERVIÇOS INICIADOS COM SUCESSO!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "${BLUE}Chat:${NC}        http://localhost:$PORT_CHAT"
    echo -e "${BLUE}Embeddings:${NC}  http://localhost:$PORT_EMBED"
    echo -e "${BLUE}PIDs:${NC}       Chat=$CHAT_PID, Embed=$EMBED_PID"
    echo -e "${BLUE}Logs:${NC}       $LOG_DIR/"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # Mostra status das GPUs
    echo -e "${YELLOW}Status das GPUs:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader
}

# Função para parar serviços
stop_services() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Parando Llama.cpp Services${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # Tenta parar de forma graciosa
    if [ -f $PID_FILE ]; then
        read CHAT_PID EMBED_PID < $PID_FILE
        echo -e "${YELLOW}Parando processos (PID: $CHAT_PID, $EMBED_PID)...${NC}"
        kill $CHAT_PID $EMBED_PID 2>/dev/null
        rm -f $PID_FILE
        sleep 2
    fi

    # Força parada se ainda estiver rodando
    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${YELLOW}Forçando parada...${NC}"
        pkill -f "llama-server.*808[0-1]" 2>/dev/null
        sleep 2
    fi

    # Verifica se parou
    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${RED}ERRO: Não foi possível parar todos os serviços${NC}"
        echo -e "Tente: pkill -9 -f 'llama-server.*808'"
        return 1
    fi

    echo -e "${GREEN}✅ Serviços parados com sucesso!${NC}"
    echo ""

    # Mostra GPUs após parar
    echo -e "${YELLOW}Status das GPUs após parada:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader
}

# Função para mostrar status
show_status() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Status dos Serviços${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # Chat
    if is_running $PORT_CHAT; then
        PID=$(get_pid $PORT_CHAT)
        echo -e "Chat (porta $PORT_CHAT):        ${GREEN}RODANDO${NC} (PID: $PID)"
    else
        echo -e "Chat (porta $PORT_CHAT):        ${RED}PARADO${NC}"
    fi

    # Embeddings
    if is_running $PORT_EMBED; then
        PID=$(get_pid $PORT_EMBED)
        echo -e "Embeddings (porta $PORT_EMBED):  ${GREEN}RODANDO${NC} (PID: $PID)"
    else
        echo -e "Embeddings (porta $PORT_EMBED):  ${RED}PARADO${NC}"
    fi

    echo ""
    echo -e "${YELLOW}GPUs:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
    echo ""

    # Testa endpoints se estiver rodando
    if is_running $PORT_CHAT; then
        echo -e "${YELLOW}Testando Chat...${NC}"
        curl -s -o /dev/null -w "  Chat: %{http_code}\n" http://localhost:$PORT_CHAT/health
    fi
    if is_running $PORT_EMBED; then
        echo -e "${YELLOW}Testando Embeddings...${NC}"
        curl -s -o /dev/null -w "  Embeddings: %{http_code}\n" http://localhost:$PORT_EMBED/health
    fi
}

# Função para mostrar logs
show_logs() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Logs em Tempo Real${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo -e "${YELLOW}Pressione Ctrl+C para sair${NC}"
    echo ""
    
    if [ ! -f "$LOG_DIR/llama-chat.log" ] || [ ! -f "$LOG_DIR/llama-embed.log" ]; then
        echo -e "${RED}Logs não encontrados. Inicie os serviços primeiro.${NC}"
        return 1
    fi
    
    tail -f $LOG_DIR/llama-chat.log $LOG_DIR/llama-embed.log
}

# Função para monitorar
show_monitor() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Monitoramento${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo -e "${YELLOW}Pressione Ctrl+C para sair${NC}"
    echo ""
    
    while true; do
        clear
        echo -e "${BLUE}=== Llama.cpp Monitor ===${NC}"
        echo -e "Data: $(date)"
        echo ""
        
        # Status dos serviços
        if is_running $PORT_CHAT; then
            echo -e "Chat (8080):        ${GREEN}✅ RODANDO${NC}"
        else
            echo -e "Chat (8080):        ${RED}❌ PARADO${NC}"
        fi
        
        if is_running $PORT_EMBED; then
            echo -e "Embeddings (8081):  ${GREEN}✅ RODANDO${NC}"
        else
            echo -e "Embeddings (8081):  ${RED}❌ PARADO${NC}"
        fi
        
        echo ""
        echo -e "${YELLOW}=== GPUs ===${NC}"
        nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu --format=csv,noheader
        
        echo ""
        echo -e "${YELLOW}=== Últimas 5 linhas do Chat ===${NC}"
        tail -5 $LOG_DIR/llama-chat.log 2>/dev/null || echo "Sem logs"
        
        echo ""
        echo -e "${YELLOW}=== Últimas 5 linhas do Embeddings ===${NC}"
        tail -5 $LOG_DIR/llama-embed.log 2>/dev/null || echo "Sem logs"
        
        sleep 3
    done
}

# Função para reiniciar
restart_services() {
    echo -e "${YELLOW}Reiniciando serviços...${NC}"
    stop_services
    sleep 2
    start_services
}

# Main
case "$1" in
    start)
        start_services
        ;;
    stop)
        stop_services
        ;;
    restart)
        restart_services
        ;;
    status)
        show_status
        ;;
    logs)
        show_logs
        ;;
    monitor)
        show_monitor
        ;;
    *)
        show_help
        ;;
esac
