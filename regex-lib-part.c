#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>
#include <ctype.h> // isalpha

#define ESC_CHAR '\\'  // экранирующий символ

const char wildcards[] = "^$.*+?[]{}|()"; // метасимволы кроме ESC_CHAR
const char quantifiers[] = "*+?{";
const char char_classes[] = "dwsapDWSAP";
const char esc_seqs[] = "rnt";
#define IS_WILDCARD(elem) (elem ? strchr(wildcards, elem) != NULL : false)
#define IS_QUANTIF(elem) (elem ? strchr(quantifiers, elem) != NULL : false)
#define IS_CHARCLASS(elem)  (elem ? strchr(char_classes, elem) != NULL : false)
#define IS_CLASS(elem) (elem ? (strchr(char_classes, elem) != NULL || strchr(esc_seqs, elem) != NULL) : false)

#define STRICT_CHARCLASSES // не разрешать экранирование букв, кроме используемых в символьных классах (как в РВ в др. языках)

enum block_type {
    PLAIN, // символы воспринимаются буквально, rep_* игнорируются
    ONE, // 1 символ, повторяющийся rep_* раз
    ANY, // .
    CHARCLASS, // символьный класс, хранит символ-описание после ESC_CHAR
    GROUP, // [...]
    REVGROUP, // [^...]
    CHAIN, // группа в круглых скобках, повторяющаяся rep_* раз
    BACKREF // обратная ссылка на захваченную группу вида \1
};

enum re_mods {
    RE_ROOT = 1 << 0, // флаг корня РВ (задан всегда)
    RE_M = 1 << 1, // ^ $ означают начало и конец каждой строки
    RE_S = 1 << 2, // . - новая строка
    RE_I = 1 << 3, // поиск без учёта регистра
    RE_IRU = 1 << 4, // без учёта регистра с поддержкой кириллицы, формат строки строго UTF-8
};

union re_data {
    char* text; // для PLAIN, GROUP, REVGROUP - указатель на malloc-строку
    char sym; // для ONE, CHARCLASS - сам символ
    struct re_chain* subchain; // для CHAIN - указатель на цепочку блоков в группе внутри круглых скобок
    int cap_num; // для BACKREF - № группы от 1 до 9 
};

// список цепочек блоков РВ (для альтерации)
// порядок рассмотрения не важен (можно добавлять в конец), достаточно разобрать одну из них
// каждая цепочка - список блоков, все из которых необходимо разобрать по очереди для успеха
struct re_chain {
    struct re_chain* next; // следующий элемент цепочки
    bool incl_start; // в РВ в других языках в каждой альтерации могут быть эти флаги
    bool incl_end;
    struct re_block* head; // начало списка блоков (для прохода с начала)
    struct re_block* tail; // конец списка (для быстрого добавления)
    int bl_count; // число блоков в списке (в текущей альтерации)
    int ch_data; // для главной цепочки: число групп захвата в РВ, иначе - № группы, к к-рой отн звено
    int flags; // для главной цепочки: модификаторы РВ (не бывает 0), иначе - 0 (признак корневой цепочки)
};

struct re_block {
    enum block_type type;
    union re_data data;
    int rep_min; // мин число повторов блока (* - 0, + - 1, ? - 0, {3-5} - 3)
    int rep_max; // макс число повторов блока (* - беск (INT_MAX), + - беск, ? - 1, {3-5} - 5)
    bool lazy; // ленивый режим захвата вместо жадного
    struct re_block* next;
};

struct re_matches {
    int* starts; // массивы длиной в capacity
    int* lengths;
    int capacity; // равна кол-ву цепочек в функции match(), иначе равна 10 (макс № обратной ссылки)
    int flags; // флаги заданы только в главной цепочке, но должны быть известны и при разборе подцепочек
};

enum re_error_type {
    RE_OK = 0,
    BAD_CARET = -100,
    BAD_DOLLAR, // = -99 и т.д.
    PREV_NOT_QUANTIF,
    EMPTY_SQUARES,
    MISSING_LSQUARE,
    MISSING_RSQUARE,
    RANGE_WITH_CHARCLASS,
    BAD_ESC_CHAR,
    BAD_INSIDE_CURLY,
    BAD_CURLY_ORDER,
    HUGE_CURLY_RANGE,
    MISSING_LPAREN,
    MISSING_RPAREN,
    BAD_SQUARE_ORDER,
    BAD_CHARCLASS,
    INVALID_FLAG,
};
// вызов "исключения" (прыжок на jenv) при возникновении ошибки в шаблоне РВ во время разбора
#define RAISE(err_code) { *erp = re_str + i; longjmp(*jenv, err_code); }

// безопасный аналог strncpy
void re_strlcpy(char* dst, const char* src, size_t size)
{
    size_t len = 0;
    while (++len < size && *src)
        *dst++ = *src++;
    if (len <= size)
        *dst = '\0';
}

// Вывод фатальной ошибки в stderr и завершение программы
#define RE_ERROR(...) \
{ \
    fprintf(stderr, "FATAL ERROR: "); \
    fprintf(stderr, __VA_ARGS__); \
    fflush(stderr); \
    exit(-1); \
}

// Выделение памяти с завершением программы при её исчерпании
void* re_malloc(size_t size)
{
    void* ptr = malloc(size);
    if (ptr)
        return ptr;
    else
        RE_ERROR("Out of memory");
}

void* re_realloc(void* block, size_t size)
{
    void* ptr = realloc(block, size);
    if (ptr)
        return ptr;
    else
        RE_ERROR("Out of memory");
}

// Возвращает указатель на n-ый элемент списка блоков или NULL, если индекс вне диапазона
struct re_block* re_nth(struct re_chain* re, int n)
{
    if (re == NULL || n < 0) return NULL;
    struct re_block* cur = re->head;
    int i = 0;
    while (cur != NULL && i < n) {
        cur = cur->next;
        i++;
    }
    return cur;
}

// Добавляет новый блок в конец списка блоков РВ
void new_block(struct re_chain* re, enum block_type type, union re_data data, int rmin, int rmax, bool lazy)
{
    struct re_block* new_bl = re_malloc(sizeof(struct re_block));
    new_bl->next = NULL;
    new_bl->type = type;
    new_bl->data = data;
    new_bl->rep_min = rmin;
    new_bl->rep_max = rmax;
    new_bl->lazy = lazy;

    if (re->tail) {
        re->tail->next = new_bl;
        re->tail = new_bl;
    }
    else // пустой список
        re->head = re->tail = new_bl;
    re->bl_count++;
}

// Обрабатывает квантификатор после текущего токена РВ и создаёт новый блок
// Возвращает длину квантификатора (больше 1 для фигурных скобок)
// также способна вызывать "исключения", если в ходе распознавания шаблона выявлены ошибки
int process_quantif(struct re_chain* re, char* re_str, enum block_type type, union re_data data, jmp_buf* jenv, char** erp)
{
    int rep_min, rep_max, i;
    bool lazy = false;
    int* curr_rep = &rep_min;
    bool second_num = false;
    switch (re_str[0]) {
        case '*':
            rep_min = 0;
            rep_max = INT_MAX;
            i = 1;
            break;
        case '+':
            rep_min = 1;
            rep_max = INT_MAX;
            i = 1;
            break;
        case '?':
            rep_min = 0;
            rep_max = 1;
            i = 1;
            break;
        case '{':
            rep_min = 0, rep_max = INT_MAX, i = 1;
            while ((re_str[i] >= '0' && re_str[i] <= '9') || re_str[i] == ',') {
                if (re_str[i] == ',') {
                    if (!second_num) {
                        second_num = true;
                        curr_rep = &rep_max;
                        if (re_str[i+1] != '}') // далее указано второе число, ограничивающее диапазон
                            rep_max = 0;
                    }
                    else
                        RAISE(BAD_INSIDE_CURLY);
                }
                else {
                    if (*curr_rep >= (INT_MAX - (int)(re_str[i] - '0')) / 10) // защита от переполнения
                        RAISE(HUGE_CURLY_RANGE);
                    *curr_rep = *curr_rep * 10 + (int)(re_str[i] - '0');
                }
                i++;
            }
            if (!second_num)
                rep_max = rep_min;
            if (re_str[i] != '}')
                RAISE(BAD_INSIDE_CURLY);
            if (rep_min > rep_max)
                RAISE(BAD_CURLY_ORDER);
            i++; // + закрывающая скобка
            break;
        default:
            rep_min = 1;
            rep_max = 1;
            i = 0;
    }
    if (re_str[i] == '?') {
        lazy = true;
        i++;
    }
    new_block(re, type, data, rep_min, rep_max, lazy);
    return i;
}

// отладочный вывод списка блоков РВ в консоль
void print_re_blocks(struct re_chain* root, int depth)
{
    if (root->flags != 0 && root->flags != RE_ROOT)
        printf("Global flags: %s%s%s%s\n", (root->flags & RE_M) ? "m" : "", (root->flags & RE_S) ? "s" : "",
                (root->flags & RE_I) ? "i" : "", (root->flags & RE_IRU) ? "I" : "");
    int alt = 0;
    for (struct re_chain* re = root; re != NULL; re = re->next) {
        for (int i = 0; i < depth; i++) printf("    "); // форматирование вложенных групп
        if (root->next) // если в РВ несколько альтераций
            printf("Alt %d, ", alt);
        printf("Flags: %c%c, %d blocks%s",
                re->incl_start ? '^' : '_', re->incl_end ? '$' : '_', re->bl_count, re->bl_count > 0 ? ": " : "");
        struct re_block* cur = re->head;
        char repeats[32];
        while (cur) {
            if (cur->rep_max == INT_MAX)
                snprintf(repeats, sizeof(repeats), "{%s%d-INF}", cur->lazy ? "~" : "", cur->rep_min);
            else if (cur->rep_min == cur->rep_max)
                snprintf(repeats, sizeof(repeats), "{%s%d}", cur->lazy ? "~" : "", cur->rep_min);
            else
                snprintf(repeats, sizeof(repeats), "{%s%d-%d}", cur->lazy ? "~" : "", cur->rep_min, cur->rep_max);
            switch(cur->type) {
                case PLAIN:
                    printf("\"%s\" ", cur->data.text);
                    break;
                case ONE:
                    printf("'%c'%s ", cur->data.sym, repeats);
                    break;
                case ANY:
                    printf("ANY%s ", repeats);
                    break;
                case CHARCLASS:
                    printf("CLASS_%c%s ", cur->data.sym, repeats);
                    break;
                case GROUP:
                case REVGROUP:
                    printf( "%s[%s]%s ", cur->type == REVGROUP ? "!" : "", cur->data.text, repeats);
                    break;
                case CHAIN:
                    printf("CHAIN#%d%s:\n", cur->data.subchain->ch_data, repeats);
                    print_re_blocks(cur->data.subchain, depth + 1);
                    if (cur->next) {
                        putchar('\n');
                        for (int i = 0; i < depth; i++) printf("    ");
                        printf(".. ");
                    }
                    break;
                case BACKREF:
                    printf("BACKREF#%d%s \n", cur->data.cap_num, repeats);
                    break;
            }   
            cur = cur->next;
        }
        if (depth == 0 || re->next)
            putchar('\n');
        alt++;
    }
}

// Парсит цепочку списков блоков, для каждой альтерации создаёт для неё звено цепочки,
// параллельно проверяя шаблон регулярного выражения на корректность
// возвращает число разобранных символов или вызывает "исключение", если шаблон некорректен
int _parse_chain(struct re_chain** root, char* re_str, char* insert_seq, int cap_status, int* fl, int* ch_cnt, jmp_buf* jenv, char** erp)
{
    struct re_chain* re = re_malloc(sizeof(struct re_chain));
    memset(re, 0, sizeof(struct re_chain));
    re->ch_data = cap_status > 0 ? (*root ? (*root)->ch_data : *ch_cnt) : 0;
    re->next = *root;
    *root = re;

    int insert_len = 0;
    int i = 0;
    while (re_str[i]) {
        if (re_str[i] == '^') {
            if (i > 0) 
                RAISE(BAD_CARET);
            re->incl_start = true;
            i++;
        }
        else if (re_str[i] == '$') {
            if (!(re_str[i+1] == '\0' || re_str[i+1] == '|' || re_str[i+1] == ')'))
                RAISE(BAD_DOLLAR);
            re->incl_end = true;
            i++;
        }
        else if (re_str[i] == '.') {
            i += 1 + process_quantif(re, re_str+i+1, ANY, (union re_data){ .text = NULL }, jenv, erp);
        }
        else if (re_str[i] == '[') {
            i++;
            enum block_type bl_type = GROUP;
            if (re_str[i] == '^') {
                bl_type = REVGROUP;
                i++;
            }
            if (re_str[i] == ']')
                RAISE(EMPTY_SQUARES);

            int group_start = i;
            while (re_str[i] != ']') { // подсчёт длины группы в символах для точного выделения памяти
                if (!re_str[i])
                    RAISE(MISSING_RSQUARE);
                if (re_str[i] == ESC_CHAR)
                    i += 2;
                else i++;
            }
            char* data_str = re_malloc(i - group_start + 1);
            int data_len = 0;
            bool next_escaped = false;
            bool escaped_hyphen = false; // экранированный дефис при наличии помещается единожды в конец строки, и перед ним не может быть ESC_CHAR
            i = group_start;
            while (re_str[i]) {
                // Метасимволы в квадратных скобок, помимо трёх ниже, не имеют силы, поэтому воспринимаются буквально (так же, как и в РВ в других языках)
                // Единственные символы, которые есть смысл экранировать: ']', '-', ESC_CHAR
                if (re_str[i] == ESC_CHAR && !next_escaped) {
                    next_escaped = true;
                    if (re_str[i+1] != '-')
                        data_str[data_len++] = re_str[i];
                    i++;
                    if (IS_CHARCLASS(re_str[i]) && re_str[i+1] == '-' && re_str[i+2] != ']') {
                        i++; // место ошибки '-'
                        RAISE(RANGE_WITH_CHARCLASS);
                    }
                    continue;
                }

                if (re_str[i+1] == '-' && re_str[i+2] && re_str[i+2] != ']') {
                    int dist = re_str[i+2] == ESC_CHAR ? 3 : 2;
                    if (re_str[i+2] == ESC_CHAR && IS_CHARCLASS(re_str[i + dist])) {
                        i++;
                        RAISE(RANGE_WITH_CHARCLASS);
                    }
                    if (re_str[i] > re_str[i + dist])
                        RAISE(BAD_SQUARE_ORDER);
                    data_str[data_len++] = '-'; // для удобства в нотации записи группы сначала символ диапазона '-'
                    data_str[data_len++] = re_str[i]; // а затем его начальный и конечный символы
                    data_str[data_len++] = re_str[i + dist];
                    i += dist;
                }
                else if (re_str[i] == '-') // дефис, не относящийся к диапазону (воспринимается буквально)
                    escaped_hyphen = true;
                else if (re_str[i] == ']' && !next_escaped)
                    break;
                else
                    data_str[data_len++] = re_str[i];

                i++;
                next_escaped = false;
            }

            if (escaped_hyphen)
                data_str[data_len++] = '-';
            data_str[data_len] = '\0';
            i += 1 + process_quantif(re, re_str+i+1, bl_type, (union re_data){ .text = data_str }, jenv, erp);
        }
        else if (re_str[i] == '|') { // начало новой альтернативы (цепочки блоков)
            i++;
            // разбор нового звена цепочки (списка блоков) с текущей позиции и до конца альтерации/группы
            i += _parse_chain(root, re_str + i, insert_seq, cap_status, fl, ch_cnt, jenv, erp);
        }
        else if (re_str[i] == '(') { // начало новой группы, разбор полностью осуществляется рекурсивным вызовом
            i++;
            bool next_capturable = true;
            bool first_char = i == 1;
            if (re_str[i] == '?') {
                i++;
                if (re_str[i] == ':') { // не-захватываемая группа
                    next_capturable = false;
                    i++;
                } else {
                    while (re_str[i] != ')') { // список модификаторов
                        switch (re_str[i]) {
                        case 'm':
                            *fl |= RE_M; break;
                        case 's':
                            *fl |= RE_S; break;
                        case 'i':
                            *fl |= RE_I; break;
                        case 'I':
                            *fl |= RE_IRU; break;
                        case '\0':
                            RAISE(MISSING_RPAREN);
                        default:
                            RAISE(INVALID_FLAG);
                        }
                        i++;
                    }
                    i++; // закрывающая скобка
                    if (first_char && re_str[i] == '^') { // в порядке исключения рассм. отдельно
                        re->incl_start = true;
                        i++;
                    }
                    continue;
                }
            } else
                (*ch_cnt)++;
            struct re_chain* new_subchain = NULL;
            i += _parse_chain(&new_subchain, re_str + i, insert_seq, next_capturable ? 1 : 0, fl, ch_cnt, jenv, erp);
            i += 1 + process_quantif(re, re_str+i+1, CHAIN, (union re_data){ .subchain = new_subchain }, jenv, erp);
        }
        else if (re_str[i] == ')') { // окончание разбора группы
            if (cap_status == -1) 
                RAISE(MISSING_LPAREN);
            return i;
        }  
        else { // символы, воспринимаемые буквально
            if (re_str[i] == ESC_CHAR) {
                i++;
                if (IS_CLASS(re_str[i])) {
                    i += 1 + process_quantif(re, re_str+i+1, CHARCLASS, (union re_data){ .sym = re_str[i] }, jenv, erp);
                    continue;
                }
                else if (re_str[i] >= '1' && re_str[i] <= '9') {
                    i += 1 + process_quantif(re, re_str+i+1, BACKREF, (union re_data){ .cap_num = (re_str[i] - '0') }, jenv, erp);
                    continue;
                }
#ifdef STRICT_CHARCLASSES
                else if (isalpha(re_str[i]))
                    RAISE(BAD_CHARCLASS);
#endif
            }
            else { // эти метасимволы всегда рассматриваются совместно с другими и в норме не бывают открывающими
                if (IS_QUANTIF(re_str[i]))
                    RAISE(PREV_NOT_QUANTIF);
                if (re_str[i] == ']')
                    RAISE(MISSING_LSQUARE);
            }
            
            if (!re_str[i])
                RAISE(BAD_ESC_CHAR);
                
            if (!re_str[i+1] || IS_WILDCARD(re_str[i+1]) || (re_str[i+1] == ESC_CHAR && (IS_CLASS(re_str[i+2]) || isdigit(re_str[i+2])))) {
                bool next_is_quantif = IS_QUANTIF(re_str[i+1]);
                if (!next_is_quantif) {
                    insert_seq[insert_len] = re_str[i];
                    insert_len++;
                }
                if (insert_len > 0) {
                    char* data_str = re_malloc(insert_len+1);
                    re_strlcpy(data_str, insert_seq, insert_len+1); // срез строки от начала длиной insert_len
                    insert_len = 0;
                    new_block(re, PLAIN, (union re_data){ .text = data_str }, 1, 1, false);
                }
                if (next_is_quantif) {
                    i += 1 + process_quantif(re, re_str+i+1, ONE, (union re_data){ .sym = re_str[i] }, jenv, erp);
                }
                else i++;
            }
            else {
                insert_seq[insert_len] = re_str[i];
                i++;
                insert_len++;
            }
        }
    }
    if (cap_status >= 0)
        RAISE(MISSING_RPAREN);
    return i;
}

void re_str_tolower(char* src, char* dest, int len, int flags) // src и dest могут указывать на одну строку
{
    for (int i = 0; i < len; i++) {
        if ((flags & RE_IRU) && src[i] == (char)208 && (src[i+1] == (char)129 || (src[i+1] >= (char)144 && src[i+1] <= (char)175))) {
            if (src[i+1] == (char)129) { // буква Ё кодируется по особым правилам
                dest[i] = (char)209;
                dest[i+1] = (char)145;
            } else if (src[i+1] + 32 > (char)191) { // для символов длиной 2 байта возможный диапазон значений второго байта от 128 до 191 
                dest[i] = (char)209;
                dest[i+1] = src[i+1] - 32; // + 32 - 192 + 128
            } else {
                dest[i] = (char)208;
                dest[i+1] = src[i+1] + 32;
            }
            i++;
        } else
            dest[i] = tolower(src[i]);
    }
    dest[len] = '\0';
}

// Приведение одного байта к нижнему регистру с учётом контекста (соседних байтов, образующих юникод-символ),
// поскольку приведение всей входной строки при каждом сопоставлении очень медленное, если строка длинная, а захваты частые 
char re_char_tolower(char* context_str, int n, int flags)
{
    char ctx[2+1];
    if (!(flags & RE_IRU && context_str[n] & 0x80)) // ASCII символы
        return tolower(context_str[n]); 

    // continuation byte (10xxxxxx) означает, что str[n] - второй байт
    // leading byte (11xxxxxx) - значит первый байт
    int sym_start = ((context_str[n] & 0xC0) == 0x80 && n > 0) ? n - 1 : n;
    re_strlcpy(ctx, context_str + sym_start, sizeof(ctx));
    re_str_tolower(ctx, ctx, 2, flags);
    return ctx[n - sym_start];
}

// Приведение регистра букв к нижнему в шаблоне РВ во всех блоках, где может храниться текст
void re_tolower_inplace(struct re_chain* root, int flags)
{
    for (struct re_chain* re = root; re != NULL; re = re->next) {
        struct re_block* cur = re->head;
        while (cur) {
            if (cur->type == PLAIN || cur->type == GROUP || cur->type == REVGROUP)
                re_str_tolower(cur->data.text, cur->data.text, strlen(cur->data.text), flags);
            else if (cur->type == ONE)
                cur->data.sym = tolower(cur->data.sym);
            else if (cur->type == CHAIN)
                re_tolower_inplace(cur->data.subchain, flags);
            cur = cur->next;
        }
    }
}

// Вспомогательная функция для вывода человекочитаемого сообщения об ошибке разбора
char* get_error_msg(enum re_error_type code) {
    switch (code) {
        case (RE_OK):
            return "OK";
        case (BAD_CARET):
            return "Unescaped ^ not in the beginning of regex or group";
        case (BAD_DOLLAR):
            return "Unescaped $ not in the end of regex";
        case (PREV_NOT_QUANTIF):
            return "Preceding token is not quantifiable";
        case (EMPTY_SQUARES):
            return "Empty square brackets";
        case (MISSING_LSQUARE):
            return "Missing opening square bracket";
        case (MISSING_RSQUARE):
            return "Missing closing square bracket";
        case (RANGE_WITH_CHARCLASS):
            return "Range with character class is inacceptable";
        case (BAD_ESC_CHAR):
            return "Missing character after escape character";
        case (BAD_INSIDE_CURLY):
            return "Invalid characters inside curly brackets";
        case (BAD_CURLY_ORDER):
            return "Quantifier range is out of order";
        case (HUGE_CURLY_RANGE):
            return "Quantifier range is too large";
        case (MISSING_LPAREN):
            return "Missing opening parenthesis";
        case (MISSING_RPAREN):
            return "Missing closing parenthesis";
        case (BAD_SQUARE_ORDER):
            return "Character range is out of order";
        case (BAD_CHARCLASS):
            return "Invalid character class";
        case (INVALID_FLAG):
            return "Invalid regex modifier";
        default:
            return "Unknown regex pattern error!";
    }
}

// Экспортируемая функция -
// Освобождение памяти, занимаемой объектом регулярного выражения, созданным функциями compile_re
void free_re(struct re_chain* root)
{
    struct re_chain* ch_tmp = NULL;
    struct re_block* bl_tmp = NULL;
    struct re_chain* re = root;
    while (re) {
        struct re_block* cur = re->head;
        while (cur) {
            if (cur->type == PLAIN || cur->type == GROUP || cur->type == REVGROUP)
                free(cur->data.text);
            else if (cur->type == CHAIN)
                free_re(cur->data.subchain);
            bl_tmp = cur;
            cur = cur->next;
            free(bl_tmp);
        }
        ch_tmp = re;
        re = re->next;
        free(ch_tmp);
    }
}

// Экспортируемая функция -
// Парсит шаблон регулярного выражения, перед этим проверяя его на корректность
// При успехе возвращает объект РВ, при неудаче - NULL и заполняет код ошибки и её позицию, если даны их указатели
struct re_chain* compile_re2(char* pattern, int* errcode, int* errpos)
{
    char* insert_seq = re_malloc(strlen(pattern) + 1); // общий временный буфер для всех звеньев
    char** err_pos_ptr = re_malloc(sizeof(char*)); // необходимо выделить на куче, чтобы безопасно вернуть сюда значение при longjmp
    jmp_buf local_env;
    int ret = setjmp(local_env);
    if (ret == 0) {
        struct re_chain* root = NULL; // инициализация цепочки
        int chain_count = 0;
        int flags = RE_ROOT;
        _parse_chain(&root, pattern, insert_seq, -1, &flags, &chain_count, &local_env, err_pos_ptr);
        // парсинг завершился успешно
        root->ch_data = chain_count;
        root->flags = flags;
        free(insert_seq);
        free(err_pos_ptr);
        if (flags & (RE_I | RE_IRU)) // флаг может быть не в начале РВ, поэтому не получится делать это во время парсинга
            re_tolower_inplace(root, root->flags);
        return root;
    }
     // возникла ошибка в ходе парсинга с кодом ret
    else {
        if (errcode)
            *errcode = ret;
        if (errpos)
            *errpos = *err_pos_ptr - pattern;
    }
    free(insert_seq);
    free(err_pos_ptr);
    return NULL;
}

struct re_chain* compile_re(char* pattern) {
    return compile_re2(pattern, NULL, NULL);
}
