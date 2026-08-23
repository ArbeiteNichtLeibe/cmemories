#include "tpm2_info.hpp"

#include <iostream>
#include <iomanip>
#include <cstdlib>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_tcti_device.h>

std::string TPM2Manager::uint32_to_string(UINT32 value) {
    char str[5];
    str[0] = static_cast<char>((value >> 24) & 0xFF);
    str[1] = static_cast<char>((value >> 16) & 0xFF);
    str[2] = static_cast<char>((value >> 8) & 0xFF);
    str[3] = static_cast<char>(value & 0xFF);
    str[4] = '\0';
    return std::string(str);
}

TPMInfo TPM2Manager::get_system_tpm_info() {
    TPMInfo info;
    ESYS_CONTEXT *esys_context = nullptr;
    TSS2_TCTI_CONTEXT *tcti_context = nullptr;
    size_t size = 0;

    TSS2_RC rc = Tss2_Tcti_Device_Init(nullptr, &size, nullptr);
    if (rc != TSS2_RC_SUCCESS) {
        info.error_message = "Erro ao obter tamanho da TCTI Device.";
        return info;
    }

    tcti_context = static_cast<TSS2_TCTI_CONTEXT*>(malloc(size));
    rc = Tss2_Tcti_Device_Init(tcti_context, &size, "/dev/tpmrm0");
    if (rc != TSS2_RC_SUCCESS) {
        info.error_message = "Falha ao abrir /dev/tpmrm0.";
        free(tcti_context);
        return info;
    }

    rc = Esys_Initialize(&esys_context, tcti_context, nullptr);
    if (rc != TSS2_RC_SUCCESS) {
        info.error_message = "Falha ao inicializar ESAPI.";
        Tss2_Tcti_Finalize(tcti_context);
        free(tcti_context);
        return info;
    }

    TPMI_YES_NO moreData;
    TPMS_CAPABILITY_DATA *capabilityData = nullptr;

    // Fabricante
    rc = Esys_GetCapability(esys_context, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                            TPM2_CAP_TPM_PROPERTIES, TPM2_PT_MANUFACTURER, 1,
                            &moreData, &capabilityData);
    if (rc == TSS2_RC_SUCCESS && capabilityData != nullptr) {
        UINT32 manufacturer = capabilityData->data.tpmProperties.tpmProperty[0].value;
        info.manufacturer = uint32_to_string(manufacturer);
        Esys_Free(capabilityData);
        capabilityData = nullptr;
    }

    // Vendor String
    rc = Esys_GetCapability(esys_context, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                            TPM2_CAP_TPM_PROPERTIES, TPM2_PT_VENDOR_STRING_1, 4,
                            &moreData, &capabilityData);
    if (rc == TSS2_RC_SUCCESS && capabilityData != nullptr) {
        for (size_t i = 0; i < capabilityData->data.tpmProperties.count; ++i) {
            info.vendor_string += uint32_to_string(capabilityData->data.tpmProperties.tpmProperty[i].value);
        }
        Esys_Free(capabilityData);
    }

    Esys_Finalize(&esys_context);
    Tss2_Tcti_Finalize(tcti_context);
    free(tcti_context);

    info.success = true;
    return info;
}