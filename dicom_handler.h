#ifndef DICOM_HANDLER_H
#define DICOM_HANDLER_H

#include <string>
#include <cstdint>

// Struktura przechowująca wyciągnięte dane (taki nasz "kontener")
struct DicomMetadata {
    std::string patientId, patientName, birthDate, patientSex;
    std::string sopUid, modality, studyDate, studyDesc;
    uintmax_t fileSize;
    std::string filePath;
};

// Deklaracja głównej funkcji odczytu
bool extractDicomMetadata(const std::string& filePath, DicomMetadata& data);

#endif