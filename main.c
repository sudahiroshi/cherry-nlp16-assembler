#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPERAND 4
#define ORIGIN 0

// line をニーモニックとオペランドに分割する
// 戻り値: オペランドの数
int SplitOpcode(char *line, char **label, char **mnemonic, char **operands, int n) {
  char *colon = strchr(line, ':');
  if (colon) {
    *label = line;
    *colon = '\0';
    line = colon + 1;
  } else {
    *label = NULL;
  }

  if ((*mnemonic = strtok(line, " \t\n")) == NULL) {
    return -1;
  }
  for (int i = 0; i < n; ++i) {
    if ((operands[i] = strtok(NULL, ",\n")) == NULL) {
      return i;
    }
    operands[i] += strspn(operands[i], " \t");
  }
  return n;
}

// 文字列をすべて小文字にする
void ToLower(char *s) {
  while (s && *s) {
    *s = tolower(*s);
    s++;
  }
}

enum RegImmKind {
  kReg = 0, kImm8 = 1, kImm16 = 2
};

struct RegImm {
  enum RegImmKind kind;
  uint16_t val;
};

const char* const reg_names[16] = {
  "ir1",  "ir2", "ir3", "flag",
  "iv",   "a",   "b",   "c",
  "d",    "e",   "mem", "bank",
  "addr", "ip",  "sp",  "zr"
};

int RegNameToIndex(const char* reg_name) {
  for (int i = 0; i < 16; i++) {
    if (strcmp(reg_names[i], reg_name) == 0) {
      return i;
    }
  }
  return -1;
}

uint8_t FlagNameToBits(const char* flag_name) {
  char flag = flag_name[0];
  uint8_t mask = 0;

  if (flag_name[0] == 'n') {
    if (strcmp(flag_name + 1, "op") == 0) { // NOP
      return 0;
    }
    flag = flag_name[1];
    mask = 1;
  }

  switch (flag) {
  case 'c': return 2u | mask;
  case 'v': return 4u | mask;
  case 'z': return 6u | mask;
  case 's': return 8u | mask;
  default:
    fprintf(stderr, "unknown flag: '%c'\n", flag);
    exit(1);
  }
}

struct Instruction {
  uint8_t op, out;
  uint8_t in, imm8;
  uint16_t imm16;
};

enum DataWidth {
  kByte,
  kWord,
};

struct LabelAddr {
  const char *label;
  int pc;
};

// Back patch type
enum BPType {
  BP_ABS16,
  BP_ABS8,
};

struct Backpatch {
  int insn_idx;
  const char *label;
  enum BPType type;
};

void InitBackpatch(struct Backpatch *bp,
                   int insn_idx, const char *label, enum BPType type) {
  bp->insn_idx = insn_idx;
  bp->label = label;
  bp->type = type;
}

// i 番目のオペランドを文字列として取得
char *GetOperand(char *mnemonic, char **operands, int n, int i) {
  if (n <= i) {
    fprintf(stderr, "too few operands for '%s': %d\n", mnemonic, n);
    exit(1);
  }
  return operands[i];
}

// i 番目のオペランドをレジスタ番号として取得
int GetOperandReg(char *operand) {
  return RegNameToIndex(operand);
}

// i 番目のオペランドを RegImm として取得
struct RegImm GetOperandRegImm(char *operand, struct Backpatch *backpatches,
                               int *num_backpatches, int insn_idx) {
  char *prefix = strtok(operand, " \t");
  char *value = strtok(NULL, " \t");
  if (value == NULL) {
    value = prefix;
    prefix = NULL;
  }

  int v;
  struct RegImm ri = {kReg, 0};
  if (prefix == NULL && (v = RegNameToIndex(value)) != -1) {
    ri.val = v;
    return ri;
  }

  char *endptr;
  v = strtol(value, &endptr, 0);

  if (prefix) {
    ToLower(prefix);
    if (strcmp(prefix, "word") == 0) {
      ri.kind = kImm16;
    } else if (strcmp(prefix, "byte") == 0) {
      ri.kind = kImm8;
    } else {
      fprintf(stderr, "unknown prefix: '%s'\n", prefix);
      exit(1);
    }
  }

  if (endptr == value) {
    if (prefix == NULL) {
      fprintf(stderr, "prefix must be given for a label: '%s'\n", value);
      exit(1);
    }
    InitBackpatch(backpatches + *num_backpatches, insn_idx, strdup(value),
                  ri.kind == kImm8 ? BP_ABS8 : BP_ABS16);
    (*num_backpatches)++;
    ri.val = 0;
  } else if (*endptr) {
    fprintf(stderr, "failed conversion to long: '%s'\n", endptr);
    exit(1);
  } else { // 正常に整数へ変換できた
    ri.val = (uint16_t)v;
    if (prefix == NULL) {
      if (0 <= v && v <= 255) {
        ri.kind = kImm8;
      } else {
        ri.kind = kImm16;
      }
    }
  }

  return ri;
}

#define GET_REG(i) GetOperandReg(\
    GetOperand((mnemonic), (operands), (num_opr), (i)))
#define GET_REGIMM(i) GetOperandRegImm(\
    GetOperand((mnemonic), (operands), (num_opr), (i)), \
    (backpatches), &(num_backpatches), (insn_idx))

void SetImm(struct Instruction *insn, enum RegImmKind imm_kind, uint16_t v) {
  if (imm_kind == kImm8) {
    insn->imm8 = v;
  } else if (imm_kind == kImm16) {
    insn->imm16 = v;
  }
}

// 入力レジスタ番号を insn に設定する。
// 入力が即値の場合は適切な即値番号（1 or 2）と即値を設定する。
//
// 戻り値
// -1: 入力が両方とも 8 ビットで表せない大きな数値である
// 2: 入力に byte リテラルが高々 1 つだけある
// 3: 入力に word リテラルが含まれる
int SetInput(struct Instruction *insn, struct RegImm *in1, struct RegImm *in2) {
  if (in2 == NULL) {
    if (in1->kind == kReg) {
      insn->in = in1->val << 4;
      return 2;
    } else {
      insn->in = in1->kind << 4;
      SetImm(insn, in1->kind, in1->val);
      return in1->kind;
    }
  }

  if (in1->kind == kReg && in2->kind == kReg) {
    insn->in = (in1->val << 4) | in2->val;
    return 2;
  } else if (in1->kind == kReg && in2->kind != kReg) {
    insn->in = (in1->val << 4) | in2->kind;
    SetImm(insn, in2->kind, in2->val);
    return 1 + in2->kind;
  } else if (in1->kind != kReg && in2->kind == kReg) {
    insn->in = (in1->kind << 4) | in2->val;
    SetImm(insn, in1->kind, in1->val);
    return 1 + in1->kind;
  } else { // in1, in2 両方が即値
    if (in1->kind == kImm16 && in2->kind == kImm16) {
      return -1;
    }
    if (in1->kind == kImm8) {
      insn->in = (kImm8 << 4) | kImm16;
      SetImm(insn, kImm8, in1->val);
      SetImm(insn, kImm16, in2->val);
    } else {
      insn->in = (kImm16 << 4) | kImm8;
      SetImm(insn, kImm16, in1->val);
      SetImm(insn, kImm8, in2->val);
    }
    return 3;
  }
}

int main(int argc, char **argv) {
  char line[256], line0[256];
  char *label;
  char *mnemonic;
  char *operands[MAX_OPERAND];

  struct Instruction insn[1024];
  int insn_idx = 0;
  int pc = ORIGIN;

  struct Backpatch backpatches[128];
  int num_backpatches = 0;

  struct LabelAddr labels[128];
  int num_labels = 0;

  memset(insn, 0, sizeof(insn));

  while (fgets(line, sizeof(line), stdin) != NULL) {
    strcpy(line0, line);
    int num_opr = SplitOpcode(line, &label, &mnemonic, operands, MAX_OPERAND);

    if (label) {
      labels[num_labels].label = strdup(label);
      labels[num_labels].pc = pc;
      num_labels++;
    }

    if (num_opr < 0) {
      continue;
    }
    ToLower(mnemonic);

    char *sep = strchr(mnemonic, '.');
    uint8_t flag = 1; // always do
    if (sep) {
      char *flag_name = sep + 1;
      *sep = '\0';
      flag = FlagNameToBits(flag_name);
    }

    int insn_len = 2;
    if (strcmp(mnemonic, "add") == 0) {
      insn[insn_idx].op = 0x12;
      insn[insn_idx].out = (flag << 4) | GET_REG(0);
      struct RegImm in1 = GET_REGIMM(1);
      struct RegImm in2 = GET_REGIMM(2);
      insn_len = SetInput(insn + insn_idx, &in1, &in2);
      if (insn_len == -1) {
        fprintf(stderr, "both literals are imm16: %s\n", line0);
        exit(1);
      }
    } else if (strcmp(mnemonic, "mov") == 0) {
      insn[insn_idx].op = 0x00;
      insn[insn_idx].out = (flag << 4) | GET_REG(0);
      struct RegImm in = GET_REGIMM(1);
      insn_len = SetInput(insn + insn_idx, &in, NULL);
    } else {
      fprintf(stderr, "unknown mnemonic: '%s'\n", mnemonic);
      exit(1);
    }

    pc += insn_len;
    insn_idx++;
  }

  for (int i = 0; i < num_backpatches; i++) {
    int l = 0;
    for (; l < num_labels; l++) {
      if (strcmp(backpatches[i].label, labels[l].label)) {
        continue;
      }

      struct Instruction *target_insn = insn + backpatches[i].insn_idx;
      switch (backpatches[i].type) {
      case BP_ABS16:
        target_insn->imm16 = labels[l].pc;
        break;
      case BP_ABS8:
        if (labels[l].pc >= 256) {
          fprintf(stderr, "label cannot be fit in imm8: '%s' -> %d\n",
                  labels[l].label, labels[l].pc);
          exit(1);
        }
        target_insn->imm8 = labels[l].pc;
        break;
      }
      break;
    }
    if (l == num_labels) {
      fprintf(stderr, "unknown label: %s\n", backpatches[i].label);
      exit(1);
    }
  }

  int debug = 0;
  if (argc > 1 && strcmp(argv[1], "-d") == 0) {
    debug = 1;
  }

  pc = ORIGIN;
  for (int i = 0; i < insn_idx; i++) {
    if (debug) {
      printf("%08x: ", pc);
    }

    printf("%02X%02X%c", insn[i].op, insn[i].out, debug ? ' ' : '\n');
    printf("%02X%02X%c", insn[i].in, insn[i].imm8, debug ? ' ' : '\n');
    pc += 2;
    if ((insn[i].in & 0xf0u) == 0x20 || (insn[i].in & 0x0fu) == 0x02) {
      printf("%04X%c", insn[i].imm16, debug ? ' ' : '\n');
      pc++;
    }

    if (debug) {
      printf("\n");
    }
  }
  return 0;
}
