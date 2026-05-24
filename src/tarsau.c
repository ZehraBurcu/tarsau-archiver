#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>

// İzin verilen maksimum dosya sayısı
#define MAX_FILES 32
// İzin verilen maksimum toplam dosya boyutu (200 MB)
#define MAX_TOTAL_SIZE 209715200

/**
 * Dosyanın yalnızca ASCII metin karakterlerinden (1 byte/char)
 * oluşup oluşmadığını kontrol eder. Null (0x00) ve 127 üzeri byte'lar
 * binary kabul edilir.
 * 
 * @param filename Kontrol edilecek dosyanın adı
 * @return Dosya metin (text) ise true, aksi halde false
 */
bool is_text_file(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return false;
    
    unsigned char buf[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            if (buf[i] > 127 || buf[i] == '\0') {
                close(fd);
                return false;
            }
        }
    }
    close(fd);
    return true;
}

/**
 * Verilen dosya veya dizin yolundaki (path) üst dizinleri oluşturur.
 * Linux 'mkdir -p' komutunun mantığıyla çalışır.
 * 
 * @param path İçinde dizin yapısı barındıran yol
 */
void create_directories(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *p = tmp;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            // Zaten varsa başarısız olur ama devam etmemizde sorun yok
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/**
 * Belirtilen dosyaları .sau uzantılı özel formattaki bir arşive sıkıştırmadan ekler.
 * 
 * @param files Arşivlenecek dosyaların listesi
 * @param file_count Listelenen dosya adedi
 * @param output_file Oluşturulacak arşiv dosyasının adı
 */
void archive_files(char *files[], int file_count, const char *output_file) {
    char metadata[8192] = ""; 
    long total_size = 0;
    
    for (int i = 0; i < file_count; i++) {
        struct stat st;
        // Dosyanın istatistik bilgilerine eriş
        if (stat(files[i], &st) < 0) {
            fprintf(stderr, "%s dosyasina erisilemiyor!\n", files[i]);
            exit(EXIT_FAILURE);
        }
        
        // Klasör vb. değil, düzenli dosya olup olmadığını ve formatını denetle
        if (!S_ISREG(st.st_mode) || !is_text_file(files[i])) {
            fprintf(stdout, "%s giriş dosyasının formatı uyumsuzdur!\n", files[i]);
            exit(EXIT_FAILURE);
        }
        
        total_size += st.st_size;
        if (total_size > MAX_TOTAL_SIZE) {
            fprintf(stdout, "Toplam dosya boyutu 200 MB'ı aşamaz!\n");
            exit(EXIT_FAILURE);
        }
        
        // Dosya kaydını (metadata) string e ekle
        char record[512];
        snprintf(record, sizeof(record), "|%s,%04o,%ld", files[i], st.st_mode & 07777, (long)st.st_size);
        strcat(metadata, record);
    }
    // Son metadata kaydını kapat
    strcat(metadata, "|");
    
    // Çıktı arşiv dosyasını oluştur (önceden varsa üstüne yazar)
    int out_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        perror("Arşiv dosyası oluşturulamadı");
        exit(EXIT_FAILURE);
    }
    
    // İlk 10 bayta organizasyon (metadata) boyutu yaz
    char header[11];
    snprintf(header, sizeof(header), "%010d", (int)strlen(metadata));
    if (write(out_fd, header, 10) != 10) {
        perror("Arşiv dosyasına yazılamadı");
        close(out_fd);
        exit(EXIT_FAILURE);
    }
    
    // Metadata'yı yaz
    if (write(out_fd, metadata, strlen(metadata)) != (ssize_t)strlen(metadata)) {
        perror("Metadata yazılamadı");
        close(out_fd);
        exit(EXIT_FAILURE);
    }
    
    // Dosya içeriklerini peşi sıra (binary güvenliğiyle de okunabilir ama sadece metin kabul ediyoruz) yaz
    for (int i = 0; i < file_count; i++) {
        int in_fd = open(files[i], O_RDONLY);
        if (in_fd < 0) continue; 
        
        char buf[4096];
        ssize_t b;
        while ((b = read(in_fd, buf, sizeof(buf))) > 0) {
            if (write(out_fd, buf, b) != b) {
                perror("İçerik arşive yazılamadı");
                close(in_fd);
                close(out_fd);
                exit(EXIT_FAILURE);
            }
        }
        close(in_fd);
    }
    
    close(out_fd);
    fprintf(stdout, "Dosyalar birleştirildi.\n");
}

/**
 * Belirtilen .sau dosyasını okuyarak hedef dizine içerisindeki dosyaları çıkarır
 * ve eski izinlerini korur.
 * 
 * @param archive_file Okunacak .sau arşivi
 * @param target_dir Çıkarım işleminin yapılacağı hedef dizin
 */
void extract_files(const char *archive_file, const char *target_dir) {
    int fd = open(archive_file, O_RDONLY);
    if (fd < 0) {
        fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
        exit(EXIT_FAILURE);
    }
    
    // İlk 10 baytı (header) oku
    char header[11];
    if (read(fd, header, 10) != 10) {
        fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
        close(fd);
        exit(EXIT_FAILURE);
    }
    header[10] = '\0';
    
    // Header'ın numeric olup olmadığını denetle
    for (int i = 0; i < 10; i++) {
        if (!isdigit(header[i])) {
            fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
            close(fd);
            exit(EXIT_FAILURE);
        }
    }
    
    // Metadata boyutunu dönüştür
    int metadata_size = atoi(header);
    if (metadata_size <= 0 || metadata_size > 10000000) {
        fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
        close(fd);
        exit(EXIT_FAILURE);
    }
    
    // Metadata için dinamik bellek ayır
    char *metadata = malloc(metadata_size + 1);
    if (!metadata) {
        fprintf(stdout, "Bellek ayırma hatası!\n");
        close(fd);
        exit(EXIT_FAILURE);
    }
    
    if (read(fd, metadata, metadata_size) != metadata_size) {
        fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
        free(metadata);
        close(fd);
        exit(EXIT_FAILURE);
    }
    metadata[metadata_size] = '\0';
    
    // Format gereği metadata '|' ile başlamalı
    if (metadata[0] != '|') {
        fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
        free(metadata);
        close(fd);
        exit(EXIT_FAILURE);
    }
    
    char *p = metadata + 1;
    // Bütün metadata kayıtlarını sırayla parse et
    while (*p != '\0') {
        char *next_pipe = strchr(p, '|');
        if (!next_pipe) {
            break; // Format sonu
        }
        
        *next_pipe = '\0'; // Stringi parçala
        
        char *comma1 = strchr(p, ',');
        if (!comma1) {
            fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
            break;
        }
        *comma1 = '\0';
        char *filename = p;
        
        char *comma2 = strchr(comma1 + 1, ',');
        if (!comma2) {
            fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
            break;
        }
        *comma2 = '\0';
        
        char *perms_str = comma1 + 1;
        char *size_str = comma2 + 1;
        
        // Verileri sayısal değerlere dönüştür
        long perms = strtol(perms_str, NULL, 8);
        long size = strtol(size_str, NULL, 10);
        
        char full_path[4096];
        if (strcmp(target_dir, ".") == 0 || strcmp(target_dir, "") == 0) {
             snprintf(full_path, sizeof(full_path), "%s", filename);
        } else {
             snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, filename);
        }
        
        // Hedef konumun dizin ağacını güvenceye al (gerekirse mkdir)
        create_directories(full_path);
        
        // Çıkarılacak dosyayı oluştur
        int out_fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0) {
            fprintf(stdout, "Arşiv dosyası uygunsuz veya bozuk!\n");
            free(metadata);
            close(fd);
            exit(EXIT_FAILURE);
        }
        
        // Arşivden 'size' bayt kadar oku ve yeni dosyaya yaz
        long bytes_left = size;
        char buf[4096];
        while (bytes_left > 0) {
            long to_read = bytes_left > (long)sizeof(buf) ? (long)sizeof(buf) : bytes_left;
            ssize_t r = read(fd, buf, to_read);
            if (r <= 0) break; 
            if (write(out_fd, buf, r) != r) {
                close(out_fd);
                free(metadata);
                close(fd);
                exit(EXIT_FAILURE);
            }
            bytes_left -= r;
        }
        close(out_fd);
        
        // İzinleri eski haline döndür
        if (chmod(full_path, perms) < 0) {
            // İzin atanırken hata olursa sessiz kal
        }
        
        // Bir sonraki dosyaya geç
        p = next_pipe + 1;
    }
    
    free(metadata);
    close(fd);
}

int main(int argc, char *argv[]) {
    // Argüman sayısı en az komut, mod (-b/-a) ve bir dosya içermelidir
    if (argc < 2) {
        fprintf(stderr, "Kullanım:\n");
        fprintf(stderr, "  Arşivleme: tarsau -b dosya1.txt dosya2.dat -o arsiv.sau\n");
        fprintf(stderr, "  Çıkarma:   tarsau -a arsiv.sau [hedef_dizin]\n");
        return EXIT_FAILURE;
    }
    
    // Arşivleme Modu
    if (strcmp(argv[1], "-b") == 0) {
        char *files[MAX_FILES];
        int file_count = 0;
        char *output_file = "a.sau";
        
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 < argc) {
                    output_file = argv[i+1];
                    i++; // dosya adını atla
                } else {
                    fprintf(stdout, "-o parametresinden sonra dosya adı belirtilmeli.\n");
                    return EXIT_FAILURE;
                }
            } else {
                if (file_count >= MAX_FILES) {
                    fprintf(stdout, "Maksimum %d dosya desteklenir.\n", MAX_FILES);
                    return EXIT_FAILURE;
                }
                files[file_count++] = argv[i];
            }
        }
        
        if (file_count == 0) {
            fprintf(stdout, "Arşivlenecek dosya belirtilmedi.\n");
            return EXIT_FAILURE;
        }
        
        archive_files(files, file_count, output_file);
        
    // Çıkarma Modu
    } else if (strcmp(argv[1], "-a") == 0) {
        if (argc < 3) {
            fprintf(stdout, "Arşiv dosyası eksik. Kullanım: tarsau -a arsiv.sau [hedef_dizin]\n");
            return EXIT_FAILURE;
        }
        
        char *archive_file = argv[2];
        char *target_dir = ".";
        
        if (argc >= 4) {
            target_dir = argv[3];
        }
        
        extract_files(archive_file, target_dir);
        
    } else {
        fprintf(stdout, "Geçersiz parametre: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}
