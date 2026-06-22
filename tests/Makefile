# Makefile testow SIM - niezalezny od glownego Makefile projektu.
# Buduje i uruchamia testy jednostkowe; udostepnia cel do testow integracyjnych.

CXX = g++
CXXFLAGS = -std=c++17 -Wall -I..
LIBS = -lodbc -ldcmdata -loflog -lofstd -lz -lstdc++fs

# unit_test.cpp dolacza ../db_handler.cpp przez #include, wiec do linkowania
# potrzebny jest jeszcze obiekt ../dicom_handler.cpp.
UNIT_BIN = unit_test

.PHONY: all test unit integration clean

all: unit

# --- Testy jednostkowe ---
$(UNIT_BIN): unit_test.cpp ../db_handler.cpp ../dicom_handler.cpp ../dicom_handler.h ../db_handler.h
	$(CXX) $(CXXFLAGS) unit_test.cpp ../dicom_handler.cpp -o $(UNIT_BIN) $(LIBS)

unit: $(UNIT_BIN)
	./$(UNIT_BIN)

# --- Testy integracyjne/funkcjonalne (sterowanie binarkami + baza) ---
integration:
	./run_integration.sh

# --- Wszystko ---
test: unit integration

clean:
	rm -f $(UNIT_BIN)
