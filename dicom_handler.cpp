#include "dicom_handler.h"
#include "dcmtk/dcmdata/dctk.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Prywatne funkcje czyszczące (tylko do użytku w tym pliku)
std::string cleanDicomString(std::string str) {
    std::replace(str.begin(), str.end(), '^', ' ');
    size_t pos = 0;
    while ((pos = str.find("'", pos)) != std::string::npos) { str.replace(pos, 1, "''"); pos += 2; }
    return str;
}

std::string formatDicomDate(std::string date) {
    if (date.length() == 8) { date.insert(4, "-"); date.insert(7, "-"); }
    return date.empty() ? "1900-01-01" : date;
}

// Główna funkcja wyciągająca dane z pliku
bool extractDicomMetadata(const std::string& filePath, DicomMetadata& data) {
    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(filePath.c_str());
    if (!status.good()) {
        return false; 
    }
    
    DcmDataset *dataset = fileformat.getDataset();
    
    const char *pId = nullptr;
    const char *pName = nullptr;
    const char *pMod = nullptr;
    const char *pDate = nullptr;
    const char *pDesc = nullptr;
    const char *sUid = nullptr;
    
    if (dataset->findAndGetString(DCM_PatientID, pId).good() && pId) data.patientId = pId;
    if (dataset->findAndGetString(DCM_PatientName, pName).good() && pName) data.patientName = pName;
    if (dataset->findAndGetString(DCM_Modality, pMod).good() && pMod) data.modality = pMod;
    if (dataset->findAndGetString(DCM_StudyDate, pDate).good() && pDate) data.studyDate = pDate;
    if (dataset->findAndGetString(DCM_StudyDescription, pDesc).good() && pDesc) data.studyDesc = pDesc;
    if (dataset->findAndGetString(DCM_SOPInstanceUID, sUid).good() && sUid) data.sopUid = sUid;
    
    // =========================================================================
    // NOWOŚĆ: Jeśli plik nie ma ID pacjenta lub UID badania, to NIE JEST poprawny DICOM!
    // =========================================================================
    if (data.patientId.empty() || data.sopUid.empty()) {
        return false; 
    }
    
    data.filePath = filePath;
    data.fileSize = std::filesystem::file_size(filePath);
    return true;
}