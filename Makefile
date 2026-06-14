# Kompilator i flagi
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Biblioteki (DICOM, ODBC, FileSystem)
LIBS = -lodbc -ldcmdata -loflog -lofstd -lz -lstdc++fs

# Wspolne pliki zreodlowe (Silnik bazy i DICOM)
COMMON_SRC = dicom_handler.cpp db_handler.cpp
COMMON_OBJ = $(COMMON_SRC:.cpp=.o)

# Program 1: Importer (Zarządca Bazy)
IMPORT_SRC = main_import.cpp
IMPORT_OBJ = $(IMPORT_SRC:.cpp=.o)
IMPORT_EXE = sim_importer

# Program 2: Wyszukiwarka (Przeglądarka)
SEARCH_SRC = main_search.cpp
SEARCH_OBJ = $(SEARCH_SRC:.cpp=.o)
SEARCH_EXE = sim_browser

# Cel glowny - kompiluj oba programy
all: $(IMPORT_EXE) $(SEARCH_EXE)

$(IMPORT_EXE): $(COMMON_OBJ) $(IMPORT_OBJ)
	$(CXX) $(COMMON_OBJ) $(IMPORT_OBJ) -o $@ $(LIBS)

$(SEARCH_EXE): $(COMMON_OBJ) $(SEARCH_OBJ)
	$(CXX) $(COMMON_OBJ) $(SEARCH_OBJ) -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Szybkie czyszczenie plików roboczych
clean:
	rm -f *.o $(IMPORT_EXE) $(SEARCH_EXE)