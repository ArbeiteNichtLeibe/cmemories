#!/bin/bash

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configurações
LLAMA_BIN="/home/andre/compilar/llamacpp/build/bin/llama-server"
MODEL_DIR="/home/andre/compilar/guffs"
MODEL_CHAT="$MODEL_DIR/DeepSeek-V4-Pro-Qwen3.5-9B-MTP-BF16.gguf"
MODEL_EMBED="$MODEL_DIR/nomic-embed-text-v1.5.f32.gguf"
PORT_CHAT=8080
PORT_EMBED=8081
LOG_DIR="/home/andre/logs"
PID_FILE="/tmp/llama.pids"

# ============================================
# ESTRATÉGIA:
# - CHAT: usa as duas GPUs (prioridade máxima)
# - EMBEDDING: CPU/RAM com --embeddings
# ============================================

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

is_running() {
    local port=$1
    if lsof -i :$port > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

get_pid() {
    local port=$1
    lsof -ti :$port 2>/dev/null
}

start_services() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Iniciando Llama.cpp Services${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${YELLOW}⚠️  Serviços já estão rodando!${NC}"
        echo -e "Use 'stop' primeiro ou 'restart' para reiniciar."
        return 1
    fi

    mkdir -p $LOG_DIR

    if [ ! -f "$LLAMA_BIN" ]; then
        echo -e "${RED}ERRO: Executável não encontrado: $LLAMA_BIN${NC}"
        return 1
    fi

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

    # Informações das GPUs
    echo -e "${YELLOW}Informações das GPUs:${NC}"
    nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader
    echo ""

    VRAM0=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1)
    VRAM1=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | tail -1)
    MODEL_SIZE=$(du -m "$MODEL_CHAT" | awk '{print $1}')
    EMBED_SIZE=$(du -m "$MODEL_EMBED" | awk '{print $1}')
    RAM_AVAIL=$(free -m | awk '/^Mem:/{print $7}')

    echo -e "${YELLOW}Análise de recursos:${NC}"
    echo -e "  Modelo CHAT: ${MODEL_SIZE} MB (${MODEL_SIZE/1024} GB)"
    echo -e "  Modelo EMBED: ${EMBED_SIZE} MB (${EMBED_SIZE/1024} GB)"
    echo -e "  GPU 0 (3060): ${VRAM0} MB (${VRAM0/1024} GB)"
    echo -e "  GPU 1 (2070): ${VRAM1} MB (${VRAM1/1024} GB)"
    echo -e "  RAM disponível: ${RAM_AVAIL} MB (${RAM_AVAIL/1024} GB)"
    echo ""

    # ============================================
    # ESTRATÉGIA: Chat nas GPUs, Embedding na CPU
    # ============================================

    echo -e "${GREEN}=== ESTRATÉGIA DE ALOCAÇÃO ===${NC}"
    echo ""

    # CHAT: Usa as duas GPUs (prioridade máxima)
    CHAT_RATIO0=5
    CHAT_RATIO1=3

    CHAT_ON_3060=$((MODEL_SIZE * CHAT_RATIO0 / (CHAT_RATIO0 + CHAT_RATIO1)))
    CHAT_ON_2070=$((MODEL_SIZE * CHAT_RATIO1 / (CHAT_RATIO0 + CHAT_RATIO1)))

    echo -e "${BLUE}1. CHAT (uso intensivo):${NC}"
    echo -e "   → Rodando nas duas GPUs"
    echo -e "   → Proporção: ${CHAT_RATIO0}:${CHAT_RATIO1}"
    echo -e "   → GPU 0 (3060): ~${CHAT_ON_3060} MB (${CHAT_RATIO0} partes)"
    echo -e "   → GPU 1 (2070): ~${CHAT_ON_2070} MB (${CHAT_RATIO1} partes)"
    echo -e "   ${GREEN}✓ Prioridade máxima - USO CONSTANTE${NC}"
    echo ""

    # EMBEDDING: CPU/RAM (uso esporádico)
    echo -e "${BLUE}2. EMBEDDING (uso esporádico):${NC}"
    echo -e "   → Rodando APENAS na CPU/RAM"
    echo -e "   → RAM necessária: ~${EMBED_SIZE} MB"
    echo -e "   → RAM disponível: ${RAM_AVAIL} MB"
    echo -e "   ${GREEN}✓ Usado apenas para indexação e consultas${NC}"
    echo ""

    # ============================================
    # INICIA CHAT (prioridade máxima)
    # ============================================

    echo -e "${YELLOW}[1/2] Iniciando CHAT (porta $PORT_CHAT)...${NC}"
    echo -e "  Split: GPU0=${CHAT_RATIO0}, GPU1=${CHAT_RATIO1}"

    $LLAMA_BIN \
        -m "$MODEL_CHAT" \
        -ngl 999 \
        -ts ${CHAT_RATIO0},${CHAT_RATIO1} \
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

    sleep 5
    if ! is_running $PORT_CHAT; then
        echo -e "${RED}ERRO: Chat falhou ao iniciar${NC}"
        tail -20 $LOG_DIR/llama-chat.log
        return 1
    fi

    # ============================================
    # INICIA EMBEDDING (CPU/RAM com --embeddings)
    # ============================================

    echo -e "${YELLOW}[2/2] Iniciando EMBEDDING (porta $PORT_EMBED)...${NC}"
    echo -e "  → Rodando na CPU/RAM (ngl=0)"
    echo -e "  → ZERO GPU usado - deixando tudo para o CHAT"
    echo -e "  → Flag --embeddings ATIVADA"

    $LLAMA_BIN \
        -m "$MODEL_EMBED" \
        --host 0.0.0.0 \
        --port $PORT_EMBED \
        --pooling cls \
        --embeddings \
        -ngl 0 \
        -t 8 \
        -b 512 \
        -ub 512 \
        --threads 8 \
        --threads-batch 4 \
        > $LOG_DIR/llama-embed.log 2>&1 &

    EMBED_PID=$!
    echo -e "${GREEN}✓ Embedding iniciado (PID: $EMBED_PID) - CPU/RAM${NC}"

    sleep 5
    if ! is_running $PORT_EMBED; then
        echo -e "${RED}ERRO: Embedding falhou ao iniciar${NC}"
        echo -e "${YELLOW}Últimas linhas do log:${NC}"
        tail -30 $LOG_DIR/llama-embed.log
        return 1
    fi

    echo "$CHAT_PID $EMBED_PID" > $PID_FILE

    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}✅ SERVIÇOS INICIADOS COM SUCESSO!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "${BLUE}Chat:${NC}        http://localhost:$PORT_CHAT"
    echo -e "${BLUE}Embeddings:${NC}  http://localhost:$PORT_EMBED"
    echo -e "${BLUE}PIDs:${NC}       Chat=$CHAT_PID, Embed=$EMBED_PID"
    echo -e "${BLUE}Logs:${NC}       $LOG_DIR/"
    echo -e "${BLUE}Split CHAT:${NC}  GPU0=${CHAT_RATIO0}, GPU1=${CHAT_RATIO1}"
    echo -e "${BLUE}Embedding:${NC}   CPU/RAM (ngl=0) com --embeddings"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    echo -e "${YELLOW}Status das GPUs:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader

    echo ""
    echo -e "${YELLOW}Status da RAM:${NC}"
    free -h
}

stop_services() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Parando Llama.cpp Services${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    if [ -f $PID_FILE ]; then
        read CHAT_PID EMBED_PID < $PID_FILE
        echo -e "${YELLOW}Parando processos (PID: $CHAT_PID, $EMBED_PID)...${NC}"
        kill $CHAT_PID $EMBED_PID 2>/dev/null
        rm -f $PID_FILE
        sleep 2
    fi

    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${YELLOW}Forçando parada...${NC}"
        pkill -f "llama-server.*808[0-1]" 2>/dev/null
        sleep 2
    fi

    if is_running $PORT_CHAT || is_running $PORT_EMBED; then
        echo -e "${RED}ERRO: Não foi possível parar todos os serviços${NC}"
        echo -e "Tente: pkill -9 -f 'llama-server.*808'"
        return 1
    fi

    echo -e "${GREEN}✅ Serviços parados com sucesso!${NC}"
    echo ""
    echo -e "${YELLOW}Status das GPUs:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader
}

show_status() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}    Status dos Serviços${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    if is_running $PORT_CHAT; then
        PID=$(get_pid $PORT_CHAT)
        echo -e "Chat (porta $PORT_CHAT):        ${GREEN}RODANDO${NC} (PID: $PID) [GPU]"
    else
        echo -e "Chat (porta $PORT_CHAT):        ${RED}PARADO${NC}"
    fi

    if is_running $PORT_EMBED; then
        PID=$(get_pid $PORT_EMBED)
        echo -e "Embeddings (porta $PORT_EMBED):  ${GREEN}RODANDO${NC} (PID: $PID) [CPU/RAM]"
    else
        echo -e "Embeddings (porta $PORT_EMBED):  ${RED}PARADO${NC}"
    fi

    echo ""
    echo -e "${YELLOW}GPUs:${NC}"
    nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
    echo ""
    echo -e "${YELLOW}RAM:${NC}"
    free -h
}

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

        if is_running $PORT_CHAT; then
            echo -e "Chat (8080):        ${GREEN}✅ RODANDO (GPU)${NC}"
        else
            echo -e "Chat (8080):        ${RED}❌ PARADO${NC}"
        fi

        if is_running $PORT_EMBED; then
            echo -e "Embeddings (8081):  ${GREEN}✅ RODANDO (CPU/RAM)${NC}"
        else
            echo -e "Embeddings (8081):  ${RED}❌ PARADO${NC}"
        fi

        echo ""
        echo -e "${YELLOW}=== GPUs ===${NC}"
        nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu --format=csv,noheader

        echo ""
        echo -e "${YELLOW}=== RAM ===${NC}"
        free -h | grep -E "Mem|Swap"

        echo ""
        echo -e "${YELLOW}=== Chat Log (últimas 3) ===${NC}"
        tail -3 $LOG_DIR/llama-chat.log 2>/dev/null || echo "Sem logs"

        echo ""
        echo -e "${YELLOW}=== Embedding Log (últimas 3) ===${NC}"
        tail -3 $LOG_DIR/llama-embed.log 2>/dev/null || echo "Sem logs"

        sleep 3
    done
}

restart_services() {
    echo -e "${YELLOW}Reiniciando serviços...${NC}"
    stop_services
    sleep 2
    start_services
}

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
