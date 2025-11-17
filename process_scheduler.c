#include <stdio.h>      
#include <stdlib.h>     
#include <string.h>     
#include <unistd.h>     
#include <pthread.h>    
#include <sys/time.h>   
#include <limits.h>     

// Hata kontrolü için 
#define CHECK(status) \
    if (status != 0) { \
        fprintf(stderr, "pthread hatası: %s\n", strerror(status)); \
        exit(1); \
    }

// 1. Prosesin durumları
typedef enum {
    NEW,
    READY,
    RUNNING,
    WAITING,
    TERMINATED
} ProcessState;

// 2. Prosesin bilgilerini tutacak ana yapı
typedef struct {
    int pid;
    int arrival_time;       //
    int total_cpu_time;     //
    int remaining_cpu_time; // Kalan CPU süresi (SRTF için)
    int interval_time;      // Bir CPU burst'ü süresi
    int io_time;            // I/O bekleme süresi
    int priority;           // Öncelik (0-10)
    
    ProcessState state;
    long time_in_ready_queue_start; // Aging için hazır kuyruğa girdiği zaman
    long io_end_time;               // I/O'nun biteceği saat
} Process;

// 3. Kuyruklar (Ready ve Waiting) için Linked List Düğümü
typedef struct Node {
    Process* process;
    struct Node* next;
} Node;

// 4. Kuyruk yapısının kendisi
typedef struct {
    Node* head;
    Node* tail;
    int count;
    pthread_mutex_t lock; // Thread-safe kuyruk için
} Queue;

// --- Global Değişkenler ---

#define MAX_PROCESSES 20 // Maksimum 20 prosess (piazza yorumlara göre 20 kabul ettim)

Process all_processes[MAX_PROCESSES]; // Tüm prosesleri tutan ana dizi
int total_process_count = 0;          // Dosyadan okunan toplam proses sayısı

Queue ready_queue;
Queue waiting_queue;

// Simülasyon saati (milisaniye)
long global_clock = 0;

// CPU'da o an çalışan proses (NULL ise CPU boş)
Process* running_process = NULL;
// CPU burst'ünün biteceği zaman
long burst_end_time = 0; 

// Biten proseslerin sayısı
int terminated_count = 0;

// I/O thread'inin ID'si 
pthread_t io_thread_id;

// Simülasyonun devam edip etmediğini I/O thread'ine bildirmek için
int simulation_running = 1;

// Thread'ler arası senkronizasyon için
pthread_mutex_t clock_mutex; // Global saati korumak için
pthread_mutex_t output_mutex; // Çıktıların karışmaması için

// --- Kuyruk Yönetimi Fonksiyonları ---


void init_queue(Queue* q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    // Mutex'i başlat
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        perror("Mutex init hatası");
        exit(1);
    }
}


void enqueue(Queue* q, Process* p) {
    Node* new_node = (Node*)malloc(sizeof(Node));
    if (new_node == NULL) {
        perror("Malloc hatası");
        exit(1);
    }
    new_node->process = p;
    new_node->next = NULL;

    // --- Kritik Bölge Başlangıcı ---
    pthread_mutex_lock(&q->lock);
    
    if (q->tail == NULL) { // Kuyruk boşsa
        q->head = new_node;
        q->tail = new_node;
    } else { // Kuyrukta eleman varsa
        q->tail->next = new_node;
        q->tail = new_node;
    }
    q->count++;
    
    pthread_mutex_unlock(&q->lock);
    // --- Kritik Bölge Sonu ---
}

// --- Yardımcı Fonksiyonlar ---


long get_current_time() {
    
    return global_clock;
}


 // Tüm çıktılar bu fonksiyonu kullanmalı.
 
void print_log(const char* message) {
    // --- Kritik Bölge Başlangıcı ---
    pthread_mutex_lock(&output_mutex);
    
    printf("[Clock: %ld] %s\n", get_current_time(), message);
    fflush(stdout); // Çıktının anında görünmesini sağlar
    
    pthread_mutex_unlock(&output_mutex);
    // --- Kritik Bölge Sonu ---
}


void* io_thread_function(void* arg) {
    char log_buffer[100]; // Log mesajları için

    // Simülasyon devam ettiği sürece çalışsın diye while loop'u
    while (simulation_running) {
        
        // --- WAITING Kuyruğu Kritik Bölge Başlangıcı ---
        pthread_mutex_lock(&waiting_queue.lock);

        Node* current = waiting_queue.head;
        Node* prev = NULL;

        long current_time = get_current_time();

        while (current != NULL) {
            Process* p = current->process;

            // Prosesin I/O süresi doldu mu kontrolü
            if (current_time >= p->io_end_time) {
                //  I/O bittiyse
                
                // 1. Logla 
                snprintf(log_buffer, 100, "PID %d finished I/O", p->pid);
                print_log(log_buffer); // 

                // 2. Prosesi WAITING kuyruğundan çıkar
                if (prev == NULL) { // İlk elemansa
                    waiting_queue.head = current->next;
                } else {
                    prev->next = current->next;
                }
                if (waiting_queue.tail == current) { // Son elemansa
                    waiting_queue.tail = prev;
                }
                waiting_queue.count--;
                
                Node* node_to_free = current; // Düğümü serbest bırakacağız
                
                // Bir sonrakine geç (listeyi bozmamak için)
                if (prev == NULL) {
                    current = waiting_queue.head;
                } else {
                    current = prev->next;
                }

                // 3. Kilidi AÇ (Ready kuyruğunu kilitlemeden önce)
                pthread_mutex_unlock(&waiting_queue.lock);

                // 4. Prosesi READY durumuna al
                p->state = READY;
                p->time_in_ready_queue_start = current_time; // Aging için
                
                // 5. Logla (çıktı formatına uygun)
                snprintf(log_buffer, 100, "PID %d moved to READY queue", p->pid);
                print_log(log_buffer); // [cite: 39]

                // 6. Prosesi READY kuyruğuna ekle (
                enqueue(&ready_queue, p);
                
                // 7. Düğümü serbest bırak
                free(node_to_free);

                // 8. WAITING kilidini tekrar al ve başa dön
                // (iteratorun bozulmaması için taramaya baştan başla)
                pthread_mutex_lock(&waiting_queue.lock);
                current = waiting_queue.head;
                prev = NULL;
                continue; // Döngüye baştan devam et
            }
            
            // I/O bitmediyse, listede ilerle
            prev = current;
            current = current->next;
        }

        // Kuyruğun sonuna geldinince kilidi açtı
        pthread_mutex_unlock(&waiting_queue.lock);
        // --- WAITING Kuyruğu Kritik Bölge Sonu ---

        // CPU'yu boşa yormamak için kısa bir süre uyu
        usleep(1000); // 1 ms bekle
    }
    
    return NULL;
}


void parse_input_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        perror("Girdi dosyası açılamadı");
        exit(1);
    }

    // Dosyayı satır satır oku 
    while (total_process_count < MAX_PROCESSES &&
           fscanf(file, "%d %d %d %d %d %d",
                  &all_processes[total_process_count].pid,
                  &all_processes[total_process_count].arrival_time,
                  &all_processes[total_process_count].total_cpu_time,
                  &all_processes[total_process_count].interval_time,
                  &all_processes[total_process_count].io_time,
                  &all_processes[total_process_count].priority) == 6)
    {
        // Okunan prosesin varsayılan değerlerini ayarla
        Process* p = &all_processes[total_process_count];
        p->remaining_cpu_time = p->total_cpu_time; // Başlangıçta kalan süre toplam süreye eşit 
        p->state = NEW;
        p->time_in_ready_queue_start = 0;
        p->io_end_time = 0;
        
        total_process_count++;
    }

    if (ferror(file)) {
        perror("Dosya okuma hatası");
        fclose(file);
        exit(1);
    }

    fclose(file);
}



void check_new_arrivals(long current_time) {
    char log_buffer[100];
    
    for (int i = 0; i < total_process_count; i++) {
        Process* p = &all_processes[i];
        
        // Yeni ve zamanı gelmiş bir proses mi kontrolü
        if (p->state == NEW && p->arrival_time == current_time) {
            
            // 1. "arrived" logunu bas 
            snprintf(log_buffer, 100, "PID %d arrived", p->pid);
            print_log(log_buffer);
            
            // 2. Durumunu güncelle ve READY kuyruğuna ekle
            p->state = READY;
            p->time_in_ready_queue_start = current_time; // Aging için 
            enqueue(&ready_queue, p);
            
            // 3. "moved to READY" logunu bas 
            snprintf(log_buffer, 100, "PID %d moved to READY queue", p->pid);
            print_log(log_buffer);
        }
    }
}

//aging mekanizması
void handle_aging(long current_time) {
    // --- READY Kuyruğu Kritik Bölge Başlangıcı ---
    pthread_mutex_lock(&ready_queue.lock);

    Node* current = ready_queue.head;
    while (current != NULL) {
        Process* p = current->process;
        
        // 100ms doldu mu kontrolü
        if (current_time - p->time_in_ready_queue_start >= 100) {
            if (p->priority > 0) { // En düşük 0 olabilir 
                p->priority--; // Önceliği artır (değeri azalt) 
            }
            // Sayacı sıfırla 
            p->time_in_ready_queue_start = current_time;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&ready_queue.lock);
    // --- READY Kuyruğu Kritik Bölge Sonu ---
}

//cpu ve dispatcher yönetimi
void schedule_cpu(long current_time) {
    char log_buffer[150];

    // --- BÖLÜM 1: CPU DOLU MU? DOLUYSA İŞİ BİTTİ Mİ KOntrolü ---
    if (running_process != NULL) {
        
        // Çalışan prosesin burst süresi doldu mu kontrolü 
        if (current_time >= burst_end_time) {
            Process* p = running_process;

            // Prosesin bu burst'te ne kadar çalıştığını hesaplama
            // (interval_time veya kalan süreden az olanı)
            int burst_duration = (p->remaining_cpu_time < p->interval_time) ?
                                 p->remaining_cpu_time : p->interval_time;
            
            p->remaining_cpu_time -= burst_duration;

            // İşi tamamen bitti mi kontrolü 
            if (p->remaining_cpu_time <= 0) {
                // --- PROSES BİTTİ (TERMINATED) ---
                p->state = TERMINATED;
                snprintf(log_buffer, 150, "PID %d TERMINATED", p->pid);
                print_log(log_buffer); 
                
                terminated_count++;
                
            } else {
                // --- I/O'YA GİDECEK (WAITING) 
                p->state = WAITING;
                p->io_end_time = current_time + p->io_time; // I/O bitiş zamanını ayarla
                
                snprintf(log_buffer, 150, "PID %d blocked for I/O for %d ms", p->pid, p->io_time);
                print_log(log_buffer); // [cite: 41]
                
                // WAITING kuyruğuna ekle
                enqueue(&waiting_queue, p);
            }
            
            // CPU'yu boşalt
            running_process = NULL;
        }
    }

    // --- BÖLÜM 2: CPU BOŞ MU? BOŞSA YENİ PROSES SEÇ ---
    if (running_process == NULL) {
        // --- READY Kuyruğu Kritik Bölge Başlangıcı ---
        pthread_mutex_lock(&ready_queue.lock);
        
        if (ready_queue.head == NULL) {
            // Seçilecek proses yok
            pthread_mutex_unlock(&ready_queue.lock);
            return;
        }

        // --- En iyi prosesi bul (Priority -> SRTF) ---
        Node* best_prev = NULL;
        Node* best_node = NULL;
        int min_priority = INT_MAX;
        int min_remaining_time = INT_MAX;

        Node* current = ready_queue.head;
        Node* prev = NULL;

        while (current != NULL) {
            Process* p = current->process;
            
            if (p->priority < min_priority) {
                // Daha iyi öncelik bulundu
                min_priority = p->priority;
                min_remaining_time = p->remaining_cpu_time;
                best_prev = prev;
                best_node = current;
            } else if (p->priority == min_priority) {
                // Öncelikler eşit, SRTF'ye bak 
                if (p->remaining_cpu_time < min_remaining_time) {
                    min_remaining_time = p->remaining_cpu_time;
                    best_prev = prev;
                    best_node = current;
                }
            }
            prev = current;
            current = current->next;
        }

        // En iyi prosesi buldu (best_node), şimdi kuyruktan çıkar
        if (best_prev == NULL) { // İlk eleman en iyisi
            ready_queue.head = best_node->next;
        } else {
            best_prev->next = best_node->next;
        }
        if (ready_queue.tail == best_node) { // Son eleman en iyisi
            ready_queue.tail = best_prev;
        }
        ready_queue.count--;

        pthread_mutex_unlock(&ready_queue.lock);
        // --- READY Kuyruğu Kritik Bölge Sonu ---

        // --- Seçilen prosesi çalıştır (Dispatch) ---
        Process* p_to_run = best_node->process;
        free(best_node); // Düğümü serbest bırak

        running_process = p_to_run;
        p_to_run->state = RUNNING;
        
        // Çalışacağı süreyi (burst) hesapla
        int burst_duration = (p_to_run->remaining_cpu_time < p_to_run->interval_time) ?
                             p_to_run->remaining_cpu_time : p_to_run->interval_time;
        
        burst_end_time = current_time + burst_duration; // Burst'ün ne zaman biteceğini kaydet

        // Logu bas 
        snprintf(log_buffer, 150, "Scheduler dispatched PID %d (Pr: %d, Rm: %d) for %d ms burst",
                 p_to_run->pid, p_to_run->priority, p_to_run->remaining_cpu_time, burst_duration);
        print_log(log_buffer);
    }
}


// --- Ana Fonksiyon ---
int main(int argc, char *argv[]) {
    // 1. Argüman Kontrolü
    if (argc != 2) {
        fprintf(stderr, "Kullanım: %s <input_file>\n", argv[0]); // 
        return 1;
    }

    // 2. Başlatma İşlemleri
    init_queue(&ready_queue);
    init_queue(&waiting_queue);

    if (pthread_mutex_init(&output_mutex, NULL) != 0) { 
        perror("Output mutex init hatası");
        return 1;
    }

    // 3. Girdi Dosyasını Oku
    parse_input_file(argv[1]);

    // 4. I/O Thread'ini Başlat
    int status = pthread_create(&io_thread_id, NULL, io_thread_function, NULL); 
    if (status != 0) { 
        fprintf(stderr, "I/O thread oluşturulamadı: %s\n", strerror(status));
        return 1;
    }

   

  // 5. Ana Simülasyon Döngüsü
    while (terminated_count < total_process_count) {
        
        // O anki saati al
        long current_time = get_current_time();

        // Adım A: Yeni gelen prosesleri kontrol et
        check_new_arrivals(current_time);
        
        // Adım B: Aging  mekanizmasını çalıştır
        handle_aging(current_time);
        
        // Adım C: CPU'yu  yönet
        schedule_cpu(current_time);
        
        // Saati ilerlet ve 1ms uyu
        global_clock++;
        usleep(1000); // 1 ms simülasyonu 
    }

    
    simulation_running = 0; // I/O thread'ine durmasını söyle

    // I/O thread'inin bitmesini bekle
    pthread_join(io_thread_id, NULL);

    // Mutex'leri yok et
    pthread_mutex_destroy(&ready_queue.lock);
    pthread_mutex_destroy(&waiting_queue.lock);
    pthread_mutex_destroy(&output_mutex);

    return 0;
}