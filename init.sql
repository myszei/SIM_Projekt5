-- Plik inicjalizacyjny dla Bazy Danych Systemu SIM

-- 1. Tworzenie użytkownika i bazy (UWAGA: Te dwie linie często uruchamia się z konta superusera postgres)
-- Opcjonalnie odkomentuj, jeśli uruchamiasz skrypt z zewnątrz:
-- CREATE USER sim_user WITH PASSWORD 'sim_password';
-- CREATE DATABASE sim_medical_db OWNER sim_user;

-- 2. Nadanie uprawnień (Upewnij się, że jesteś połączony z bazą sim_medical_db)
GRANT ALL ON SCHEMA public TO sim_user;

-- 3. Budowa struktury tabel
CREATE TABLE IF NOT EXISTS data_sources (
    id SERIAL PRIMARY KEY,
    directory_path VARCHAR(512) NOT NULL,
    last_scanned TIMESTAMP
);

CREATE TABLE IF NOT EXISTS patients (
    patient_id VARCHAR(100) PRIMARY KEY,
    full_name VARCHAR(255),
    birth_date DATE,
    sex VARCHAR(1)
);

CREATE TABLE IF NOT EXISTS studies (
    study_uid VARCHAR(128) PRIMARY KEY,
    patient_id VARCHAR(100) REFERENCES patients(patient_id),
    source_id INT REFERENCES data_sources(id),
    modality VARCHAR(10),
    study_date DATE,
    study_desc TEXT,
    file_path VARCHAR(512),
    file_size BIGINT,
    is_deleted BOOLEAN DEFAULT false
);

-- Zmiana właściciela tabel na użytkownika programu
ALTER TABLE data_sources OWNER TO sim_user;
ALTER TABLE patients OWNER TO sim_user;
ALTER TABLE studies OWNER TO sim_user;

-- Przekazanie praw do edycji i korzystania z liczników (SERIAL)
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA public TO sim_user;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO sim_user;
-- Informacja o zakończeniu
\echo 'Struktura bazy SIM_MEDICAL_DB zostala poprawnie utworzona!'
