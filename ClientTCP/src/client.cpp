#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <iomanip>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>

using namespace std;

#define SERVER_IP "127.0.0.1" // Замените на IP-адрес сервера

// Вычисление базовой контрольной суммы
unsigned int generateCheckSum(const string& filePath, long start, long end) {
    ifstream file(filePath, ios::binary);
    if (!file.is_open()) {
        cerr << "Не удалось открыть файл: " << filePath << endl;
        return 0;
    }

    unsigned int sum = 0;
    char buffer[1024];
    file.seekg(start, ios::beg);
    while (file.tellg() < end && file.read(buffer, sizeof(buffer))) {
        for (int i = 0; i < file.gcount(); i++) {
            sum += (unsigned char)buffer[i];
        }
    }

    return sum;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Неверный формат вызова: ./client <порт>" << endl;
        return 1;
    }
    int port = atoi(argv[1]); 
    int clientSocket;
    struct sockaddr_in serverAddr;

    // Создание сокета
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) {
        cerr << "Ошибка создания сокета" << endl;
        return 1;
    }

    // Заполнение структуры адреса
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(SERVER_IP);
    serverAddr.sin_port = htons(port);

    // Подключение к серверу
    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        cerr << "Ошибка подключения к серверу" << endl;
        return 1;
    }

    // Указываем путь к папке, из которой отправляются файлы
    string sourceFolder = "../Data"; // Замените на свой путь

    // Получаем список файлов из папки
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(sourceFolder.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            string filename = ent->d_name;
            if (filename == "." || filename == "..") continue; // Пропускаем точки

            string fullPath = sourceFolder + "/" + filename;

            // Получаем размер файла
            ifstream file(fullPath, ios::binary);
            if (!file.is_open()) {
                cerr << "Ошибка открытия файла: " << fullPath << endl;
                return 1;
            }
            file.seekg(0, ios::end);
            long fileSize = file.tellg();
            file.close();

            char readyBuffer[10];
            int byteReceived = recv(clientSocket, readyBuffer, sizeof(readyBuffer), 0);
            if (byteReceived <= 0) {
                cerr << "Ошибка получения сигнала готовности" << endl;
                return 1;
            }
            string readySignal(readyBuffer, byteReceived);
            if (readySignal != "READY") {
                cerr << "Неверный сигнал готовности" << endl;
                return 1;
            }

            // Отправляем имя файла на сервер
            if (send(clientSocket, filename.c_str(), filename.length(), 0) != filename.length()) {
                cerr << "Ошибка отправки имени файла " << filename << endl;
                return 1;
            }
            sleep(1);
            // Отправляем размер файла на сервер
            string sizeStr = to_string(fileSize);
            if (send(clientSocket, sizeStr.c_str(), sizeStr.length(), 0) != sizeStr.length()) {
                cerr << "Ошибка отправки размера файла " << filename << endl;
                return 1;
            }

            // Вычисляем контрольную сумму для файла
            unsigned int checksum = generateCheckSum(fullPath, 0, fileSize);

            // Отправляем контрольную сумму на сервер
            string checksumStr = to_string(checksum);
            checksumStr += '\0';
            if (send(clientSocket, checksumStr.c_str(), checksumStr.length(), 0) != checksumStr.length()) {
                cerr << "Ошибка отправки контрольной суммы" << endl;
                return 1;
            }
            sleep(1);
            // Открываем файл для чтения (в бинарном режиме)
            int fileDescriptor = open(fullPath.c_str(), O_RDONLY);
            if (fileDescriptor == -1) {
                cerr << "Ошибка открытия файла: " << fullPath << endl;
                return 1;
            }

            // Получаем информацию о размере файла на сервере
            char serverResponse[1024];
            int bytesReceived = recv(clientSocket, serverResponse, sizeof(serverResponse), 0);
            if (bytesReceived <= 0) {
                cerr << "Ошибка получения ответа от сервера" << endl;
                return 1;
            }
            string serverMessage(serverResponse, bytesReceived);
            
            // Проверяем ответ сервера
            if (serverMessage == "ALREADY_DOWNLOADED") {
                cout << "Файл " << filename << " уже загружен на сервере" << endl;
                close(fileDescriptor);
                continue; // Переходим к следующему файлу
            }

            long serverFileSize = 0;
            try {
                serverFileSize = stol(serverMessage);
            } catch (const invalid_argument& e) {
                cerr << "Неверный формат ответа сервера: " << serverMessage << endl;
                close(fileDescriptor);
                continue;
            } catch (const out_of_range& e) {
                cerr << "Значение размера файла вне допустимого диапазона: " << serverMessage << endl;
                close(fileDescriptor);
                continue;
            }
            cout << "Размер файла " << filename << " на сервере: " << serverFileSize << endl;
            
            // Проверяем, нужно ли докачивать файл
            long sentBytes = 0;
            if (serverFileSize < fileSize) {
                cout << "Докачиваем файл " << filename << "..." << endl;

                lseek(fileDescriptor, serverFileSize, SEEK_SET);
                char buffer[256];
                while (sentBytes < fileSize - serverFileSize) {
                    // Читаем данные из файла
                    int bytesRead = read(fileDescriptor, buffer, sizeof(buffer));
                    if (bytesRead <= 0) {
                        break;
                    }

                    // Отправляем данные на сервер
                    int bytesSent = send(clientSocket, buffer, bytesRead, 0);
                    if (bytesSent <= 0) {
                        cerr << "Ошибка отправки файла" << endl;
                        return 1;
                    }

                    sentBytes += bytesSent;
                }
            } else {
                cout << "Файл " << filename << " уже полностью загружен" << endl;
            }

            cout << "Файл " << filename << " отправлен." << endl;

            close(fileDescriptor);
        }
        closedir(dir);
    } else {
        cerr << "Ошибка открытия папки: " << sourceFolder << endl;
        return 1;
    }

    close(clientSocket);
    return 0;
}