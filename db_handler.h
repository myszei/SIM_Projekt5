#ifndef DB_HANDLER_H
#define DB_HANDLER_H

#include <sql.h>
#include <sqlext.h>

SQLHDBC connectToDatabase();
void disconnectDatabase(SQLHDBC hDbc);
void indexDirectory(SQLHDBC hDbc);  // Punkt A i C: Tylko skanowanie (bez kopiowania)
void importDirectory(SQLHDBC hDbc); // Punkt B: Kopiowanie do bazy (Archiwum)
void searchPatient(SQLHDBC hDbc);
void syncDatabase(SQLHDBC hDbc);
void showStatistics(SQLHDBC hDbc);
void listAllPatients(SQLHDBC hDbc);

#endif