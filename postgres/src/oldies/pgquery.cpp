#include "pgquery.hpp"
#include "start_connections_sql.hpp"
#include "postgres_connection.hpp"
#include "../../uteis/include/uteis.hpp"
#include <cstdio>
#include <cstring>

bool pgquery(int connectionId,
             const char* sql,
             char* outResult,
             size_t outResultSize,
             size_t& outResultLen,
             char* outError,
             size_t errSize) {
    // Validação de parâmetros
    if (!sql || !outResult || outResultSize == 0) {
        Utils::safeCopyString(outError, errSize, "Parâmetros inválidos");
        return false;
    }

    // Obter a conexão pelo ID
    PostgresConnection* conn = SqlConnectionManager::getInstance().getConnection(connectionId);
    if (!conn) {
        snprintf(outError, errSize, "Conexão %d não encontrada", connectionId);
        return false;
    }
    if (!conn->isReady()) {
        snprintf(outError, errSize, "Conexão %d não está pronta (não autenticada)", connectionId);
        return false;
    }

    // Executar a query usando o método existente
    char errBuf[256];
    if (!conn->query(sql, outResult, outResultSize, outResultLen, errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        return false;
    }

    // Garantir que o resultado seja uma string terminada em nulo
    if (outResultLen < outResultSize) {
        outResult[outResultLen] = '\0';
    } else {
        // Se o buffer encheu, truncar e marcar erro
        outResult[outResultSize - 1] = '\0';
        outResultLen = outResultSize - 1;
        Utils::safeCopyString(outError, errSize, "Resultado truncado (buffer pequeno)");
        return false;
    }

    return true;
}