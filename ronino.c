/* RONINO C64 (single file, Oscar64).
 *
 * Why this file looks compact and a bit "flat":
 * - On C64, predictability beats abstraction: explicit memory placement and
 *   simple loops make video timing and RAM usage easier to reason about.
 * - We keep logic, UI and AI together so behavior changes are localized and
 *   easy to iterate while tuning gameplay on real hardware/emulators.
 * - Pieces are charset-based on purpose: this avoids the 8-sprite limit and
 *   removes raster-splitting complexity that was hurting visual stability.
 * - The AI runs incrementally at root level so the machine can "think" without
 *   freezing the frame updates and input responsiveness.
 */
#include <c64/vic.h>
#include <c64/joystick.h>
#include <conio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>

/* Keep assets in VIC-visible bank 0 so screen/charset pointers never need
 * bank juggling at runtime, which is a common source of glitches. */
#pragma region( lower, 0x0a00, 0x2000, , , {code, data} )
#pragma section( charsetset, 0)
#pragma region( charsetset, 0x2000, 0x2800, , , {charsetset} )
#pragma region( main, 0x2800, 0x9000, , , {code, data, bss, heap, stack} )

#define BOARD_N 5
#define BOARD_CELLS (BOARD_N * BOARD_N)

#define EMPTY 0
#define H_STUDENT 1
#define H_MASTER 2
#define C_STUDENT -1
#define C_MASTER -2

#define HUMAN 1
#define AI -1
#define DRAW 2

#define MAX_MOVES 48
#define MAX_PLY 200

#define INF_SCORE 30000
#define SEARCH_DEPTH 3

/* Keep one port for the whole game to avoid title/gameplay input mismatch. */
#define JOY_PORT 1

#define Screen ((unsigned char *)0x0400)
#define Color ((unsigned char *)0xd800)

#define GRID_X 2
#define GRID_Y 6
#define CELL_W 4
#define CELL_H 3

#define CH_PAWN_TL 240
#define CH_PAWN_TR 241
#define CH_PAWN_BL 242
#define CH_PAWN_BR 243
#define CH_KING_TL 244
#define CH_KING_TM 245
#define CH_KING_TR 246
#define CH_KING_BL 247
#define CH_KING_BM 248
#define CH_KING_BR 249
#define CH_CARD_ORIGIN 250

/* We write text manually for speed and control; this map keeps screen code
 * conversion deterministic instead of relying on stdio cursor logic. */
static const unsigned char P2SMap[8] = {0x00, 0x00, 0x40, 0x20, 0x80, 0xc0, 0x80, 0x80};

/* ============================== Core Model ============================== */
typedef struct {
    const char *name;
    signed char stamp;
    signed char moveCount;
    signed char dx[4];
    signed char dy[4];
} MoveCard;

typedef struct {
    unsigned char from;
    unsigned char to;
    unsigned char cardSlot;
    unsigned char cardIdx;
    signed char piece;
    signed char captured;
} Move;

typedef struct {
    unsigned char oldNeutral;
    unsigned char oldHand0;
    unsigned char oldHand1;
    signed char oldSide;
    signed char oldWinner;
    unsigned char oldPly;
} Undo;

typedef struct {
    signed char board[BOARD_CELLS];
    unsigned char handH[2];
    unsigned char handC[2];
    unsigned char neutral;
    signed char side;
    signed char winner;
    unsigned char ply;
} GameState;

typedef struct {
    bool showCursor;
    unsigned char cursorX;
    unsigned char cursorY;
    unsigned char selectedFrom;
    unsigned char selectedCardSlot;
    const Move *movesFromSelected;
    unsigned char movesFromSelectedCount;
} HumanUI;

/* A private charset lets us shape pieces/cards without touching screen logic
 * and without the flicker tradeoffs of heavy sprite multiplexing. */
#pragma data(charsetset)
static unsigned char CharsetRAM[2048] = {0};
#pragma align(CharsetRAM, 2048);
#pragma data(data)

static const MoveCard Cards[16] = {
    {"Fangs",    -1, 2, { 0,  0, 0, 0}, {-2,  1, 0, 0}},
    {"Wyvern",    1, 4, {-2,  2,-1, 1}, {-1, -1, 1, 1}},
    {"Leapr",    -1, 3, {-1, -2, 1, 0}, {-1,  0, 1, 0}},
    {"Dart",      1, 3, { 1,  2,-1, 0}, {-1,  0, 1, 0}},
    {"Claw",     -1, 3, { 0, -2, 2, 0}, {-1,  0, 0, 0}},
    {"Titan",     1, 4, {-1,  1,-1, 1}, {-1, -1, 0, 0}},
    {"Glide",    -1, 4, {-1, -1, 1, 1}, {-1,  0, 0, 1}},
    {"Spur",      1, 4, { 1, -1, 1,-1}, {-1,  0, 0, 1}},
    {"Trick",    -1, 4, {-1, -1, 1, 1}, {-1,  1,-1, 1}},
    {"Scyth",     1, 3, {-1,  1, 0, 0}, {-1, -1, 1, 0}},
    {"Charg",    -1, 3, { 0, -1, 0, 0}, {-1,  0, 1, 0}},
    {"Ram",       1, 3, { 0,  1, 0, 0}, {-1,  0, 1, 0}},
    {"Heron",    -1, 3, { 0, -1, 1, 0}, {-1,  1, 1, 0}},
    {"Tuskr",     1, 3, { 0, -1, 1, 0}, {-1,  0, 0, 0}},
    {"Slink",    -1, 3, {-1,  1,-1, 0}, {-1,  0, 1, 0}},
    {"Viper",     1, 3, {-1,  1, 1, 0}, { 0, -1, 1, 0}},
};

typedef struct {
    bool active;
    GameState probe;
    Move rootMoves[MAX_MOVES];
    unsigned char rootCount;
    unsigned char rootIndex;
    Move bestMove;
    int bestScore;
} AIThinkState;

/* Keep all mutable runtime state in a single object so reasoning about state
 * transitions is local and side effects are explicit. */
typedef struct {
    bool running;
    bool charsetReady;
    unsigned char cursorX;
    unsigned char cursorY;
    unsigned char searchPulse;
    GameState game;
    AIThinkState ai;
    Move searchRaw[SEARCH_DEPTH + 1][MAX_MOVES];
    Move searchSorted[SEARCH_DEPTH + 1][MAX_MOVES];
} AppState;

static AppState S = {true, false, 2, 4, 0};

/* ============================ Low Level Draw ============================ */
static inline unsigned char idxOf(unsigned char x, unsigned char y) {
    /* A flat index keeps board access branch-free and cache-friendly even on
     * tiny systems; 5x5 math is cheap and predictable. */
    return (unsigned char)(y * BOARD_N + x);
}

static inline bool inBounds(signed char x, signed char y) {
    /* Signed checks prevent underflow bugs when applying negative deltas. */
    return x >= 0 && x < BOARD_N && y >= 0 && y < BOARD_N;
}

static inline void screenPut(unsigned char x, unsigned char y, char ch, unsigned char color) {
    /* Direct screen RAM writes avoid conio cursor state and keep frame timing
     * deterministic, which matters during AI updates. */
    unsigned short p = (unsigned short)(40 * y + x);
    unsigned char c = (unsigned char)ch;
    Screen[p] = c ^ P2SMap[c >> 5];
    Color[p] = color;
}

static void clearRow(unsigned char y, unsigned char color) {
    for (unsigned char x = 0; x < 40; ++x) {
        screenPut(x, y, ' ', color);
    }
}

static void drawText(unsigned char x, unsigned char y, const char *s, unsigned char color) {
    while (*s && x < 40) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        screenPut(x++, y, c, color);
    }
}

static inline void screenPutCode(unsigned char x, unsigned char y, unsigned char code, unsigned char color) {
    unsigned short p = (unsigned short)(40 * y + x);
    Screen[p] = code;
    Color[p] = color;
}

static unsigned char pieceColor(signed char p) {
    return (p > 0) ? VCOL_RED : VCOL_LT_BLUE;
}

static void charsetDefineChar(unsigned char code, const unsigned char shape[8]) {
    memcpy(&CharsetRAM[(unsigned short)code * 8], shape, 8);
}

/* ============================ Charset Assets ============================ */
static void charsetBuildPieceChars(void) {
    /* Pawns are intentionally smaller so tactical reading is instant even on a
     * crowded board: the king must pop out at a glance. */
    static const unsigned char pawn_tl[8] = {
        0x00, 0x0c, 0x1e, 0x1e, 0x0e, 0x07, 0x03, 0x00
    };
    static const unsigned char pawn_tr[8] = {
        0x00, 0x30, 0x78, 0x78, 0x70, 0xe0, 0xc0, 0x00
    };
    static const unsigned char pawn_bl[8] = {
        0x00, 0x03, 0x0f, 0x1f, 0x1f, 0x3f, 0x3f, 0x00
    };
    static const unsigned char pawn_br[8] = {
        0x00, 0xc0, 0xf0, 0xf8, 0xf8, 0xfc, 0xfc, 0x00
    };

    /* Kings use a wider silhouette to avoid confusion during fast navigation.
     * Distinct shape matters more than aesthetic fidelity in this resolution. */
    static const unsigned char king_tl[8] = {
        0x00, 0x06, 0x0f, 0x1f, 0x3e, 0x3e, 0x3f, 0x3f
    };
    static const unsigned char king_tm[8] = {
        0x00, 0x24, 0x7e, 0x7e, 0x7e, 0x7e, 0xff, 0xff
    };
    static const unsigned char king_tr[8] = {
        0x00, 0x60, 0xf0, 0xf8, 0x7c, 0x7c, 0xfc, 0xfc
    };
    static const unsigned char king_bl[8] = {
        0x3f, 0x3f, 0x3f, 0x3f, 0x1f, 0x1f, 0x0f, 0x00
    };
    static const unsigned char king_bm[8] = {
        0xff, 0xff, 0xff, 0x7e, 0x7e, 0x7e, 0x3c, 0x00
    };
    static const unsigned char king_br[8] = {
        0xfc, 0xfc, 0xfc, 0xfc, 0xf8, 0xf8, 0xf0, 0x00
    };
    static const unsigned char card_origin[8] = {
        0x00, 0x18, 0x3c, 0x7e, 0x3c, 0x18, 0x00, 0x00
    };

    charsetDefineChar(CH_PAWN_TL, pawn_tl);
    charsetDefineChar(CH_PAWN_TR, pawn_tr);
    charsetDefineChar(CH_PAWN_BL, pawn_bl);
    charsetDefineChar(CH_PAWN_BR, pawn_br);
    charsetDefineChar(CH_KING_TL, king_tl);
    charsetDefineChar(CH_KING_TM, king_tm);
    charsetDefineChar(CH_KING_TR, king_tr);
    charsetDefineChar(CH_KING_BL, king_bl);
    charsetDefineChar(CH_KING_BM, king_bm);
    charsetDefineChar(CH_KING_BR, king_br);
    charsetDefineChar(CH_CARD_ORIGIN, card_origin);
}

static void charsetInit(void) {
    if (S.charsetReady) return;

    /* We copy ROM charset first so UI text remains stable; then we overwrite
     * only a few glyph slots for game symbols. */
    volatile unsigned char *cpuPort = (volatile unsigned char *)0x0001;
    unsigned char oldPort = *cpuPort;

    __asm { sei };
    *cpuPort = (unsigned char)(oldPort & (unsigned char)~0x04);
    memcpy(CharsetRAM, (const void *)0xd000, 2048);
    *cpuPort = oldPort;
    __asm { cli };

    charsetBuildPieceChars();

    vic_setbank(0);
    vic.ctrl2 &= (unsigned char)~VIC_CTRL2_MCM;
    vic.memptr = (unsigned char)((((unsigned)Screen >> 6) & 0xf0) | (((unsigned)CharsetRAM >> 10) & 0x0e));

    S.charsetReady = true;
}

/* ============================ Game Mechanics ============================ */
static unsigned char countPieces(const GameState *g, signed char side) {
    unsigned char c = 0;
    for (unsigned char i = 0; i < BOARD_CELLS; ++i) {
        signed char p = g->board[i];
        if ((side == HUMAN && p > 0) || (side == AI && p < 0)) {
            ++c;
        }
    }
    return c;
}

static unsigned char cellCenterX(unsigned char x) {
    return (unsigned char)(GRID_X + x * CELL_W + (CELL_W / 2));
}

static unsigned char cellCenterY(unsigned char y) {
    return (unsigned char)(GRID_Y + y * CELL_H + (CELL_H / 2));
}

static bool isMarkedDestination(const HumanUI *ui, unsigned char sq) {
    if (!ui || ui->selectedFrom == 255) return false;
    for (unsigned char i = 0; i < ui->movesFromSelectedCount; ++i) {
        if (ui->movesFromSelected[i].to == sq) return true;
    }
    return false;
}

static void drawPieces(const GameState *g) {
    for (unsigned char y = 0; y < BOARD_N; ++y) {
        for (unsigned char x = 0; x < BOARD_N; ++x) {
            signed char p = g->board[idxOf(x, y)];
            if (p == EMPTY) continue;

            unsigned char x0 = (unsigned char)(GRID_X + x * CELL_W);
            unsigned char y0 = (unsigned char)(GRID_Y + y * CELL_H);
            unsigned char col = pieceColor(p);

            if (p == H_MASTER || p == C_MASTER) {
                /* Kings use extra area so the eye can track win-critical
                 * pieces immediately, even while cursor/highlights change. */
                screenPutCode((unsigned char)(x0 + 1), (unsigned char)(y0 + 1), CH_KING_TL, col);
                screenPutCode((unsigned char)(x0 + 2), (unsigned char)(y0 + 1), CH_KING_TM, col);
                screenPutCode((unsigned char)(x0 + 3), (unsigned char)(y0 + 1), CH_KING_TR, col);
                screenPutCode((unsigned char)(x0 + 1), (unsigned char)(y0 + 2), CH_KING_BL, col);
                screenPutCode((unsigned char)(x0 + 2), (unsigned char)(y0 + 2), CH_KING_BM, col);
                screenPutCode((unsigned char)(x0 + 3), (unsigned char)(y0 + 2), CH_KING_BR, col);
            } else {
                /* Pawns stay compact so destination markers remain visible
                 * around them during move planning. */
                screenPutCode((unsigned char)(x0 + 1), (unsigned char)(y0 + 1), CH_PAWN_TL, col);
                screenPutCode((unsigned char)(x0 + 2), (unsigned char)(y0 + 1), CH_PAWN_TR, col);
                screenPutCode((unsigned char)(x0 + 1), (unsigned char)(y0 + 2), CH_PAWN_BL, col);
                screenPutCode((unsigned char)(x0 + 2), (unsigned char)(y0 + 2), CH_PAWN_BR, col);
            }
        }
    }
}

static void frameTick(void) {
    vic_waitFrame();
}

static void searchHeartbeat(void) {
    /* Periodic frame sync keeps VIC updates alive while deep search runs.
     * Without this, the machine appears frozen during long branches. */
    if (++S.searchPulse >= 24) {
        S.searchPulse = 0;
        frameTick();
    }
}

static void initBoard(GameState *g) {
    memset(g, 0, sizeof(*g));

    g->board[idxOf(0, 4)] = H_STUDENT;
    g->board[idxOf(1, 4)] = H_STUDENT;
    g->board[idxOf(2, 4)] = H_MASTER;
    g->board[idxOf(3, 4)] = H_STUDENT;
    g->board[idxOf(4, 4)] = H_STUDENT;

    g->board[idxOf(0, 0)] = C_STUDENT;
    g->board[idxOf(1, 0)] = C_STUDENT;
    g->board[idxOf(2, 0)] = C_MASTER;
    g->board[idxOf(3, 0)] = C_STUDENT;
    g->board[idxOf(4, 0)] = C_STUDENT;

    g->winner = 0;
    g->ply = 0;
}

static void shuffleCards(unsigned char *deck, unsigned char n) {
    for (unsigned char i = 0; i < n; ++i) {
        deck[i] = i;
    }
    for (signed char i = (signed char)(n - 1); i > 0; --i) {
        unsigned char j = (unsigned char)(rand() % (i + 1));
        unsigned char t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
}

static void newGame(GameState *g) {
    unsigned char deck[16];

    initBoard(g);
    shuffleCards(deck, 16);

    g->handH[0] = deck[0];
    g->handH[1] = deck[1];
    g->handC[0] = deck[2];
    g->handC[1] = deck[3];
    g->neutral = deck[4];

    /* Keep opening control in human hands for better pacing and for easier
     * manual verification while tuning AI/evaluation parameters. */
    g->side = HUMAN;

    S.ai.active = false;
}

static bool isWinningMove(const Move *m, signed char side) {
    if (m->piece == 0) return false;

    if (side == HUMAN) {
        if (m->captured == C_MASTER) return true;
        if (m->piece == H_MASTER && m->to == idxOf(2, 0)) return true;
    } else {
        if (m->captured == H_MASTER) return true;
        if (m->piece == C_MASTER && m->to == idxOf(2, 4)) return true;
    }
    return false;
}

static signed char ownerOfPiece(signed char p) {
    if (p > 0) return HUMAN;
    if (p < 0) return AI;
    return 0;
}

static unsigned char generateMoves(const GameState *g, signed char side, Move *outMoves) {
    /* Keep generation linear and explicit (piece -> card -> pattern):
     * easy to debug and fast enough on a 5x5 board. */
    unsigned char count = 0;

    /* Side has exactly two cards available this turn. */
    const unsigned char *hand = (side == HUMAN) ? g->handH : g->handC;

    /* Scan all board cells and pick only pieces owned by the side to move. */
    for (unsigned char sq = 0; sq < BOARD_CELLS; ++sq) {
        signed char piece = g->board[sq];
        if (ownerOfPiece(piece) != side) continue;

        /* Decode once here to avoid repeated modulo/division in inner loops. */
        signed char px = sq % BOARD_N;
        signed char py = sq / BOARD_N;

        /* Try both cards from hand. */
        for (unsigned char cs = 0; cs < 2; ++cs) {
            unsigned char cardIdx = hand[cs];
            const MoveCard *card = &Cards[cardIdx];

            /* Expand each movement vector from the selected card. */
            for (unsigned char mi = 0; mi < (unsigned char)card->moveCount; ++mi) {
                signed char dx = card->dx[mi];
                signed char dy = card->dy[mi];

                /* Card geometry is authored from one perspective.
                 * Mirroring for AI keeps card data compact and symmetric. */
                if (side == AI) {
                    dx = -dx;
                    dy = -dy;
                }

                signed char nx = px + dx;
                signed char ny = py + dy;
                /* Skip projections outside board limits. */
                if (!inBounds(nx, ny)) continue;

                unsigned char to = idxOf((unsigned char)nx, (unsigned char)ny);
                signed char target = g->board[to];
                /* Do not generate self-captures; everything else is legal. */
                if (ownerOfPiece(target) == side) continue;

                /* Store enough info to execute and undo in O(1) without
                 * re-reading board/card state later. */
                Move *m = &outMoves[count++];
                m->from = sq;
                m->to = to;
                m->cardSlot = cs;
                m->cardIdx = cardIdx;
                m->piece = piece;
                m->captured = target;
            }
        }
    }

    if (count == 0) {
        /* Rules require card rotation even when no piece can move.
         * We encode this as synthetic "discard" moves so search and UI can
         * stay on a single move pipeline. */
        for (unsigned char cs = 0; cs < 2; ++cs) {
            Move *m = &outMoves[count++];
            m->from = 255;
            m->to = 255;
            m->cardSlot = cs;
            m->cardIdx = hand[cs];
            m->piece = 0;
            m->captured = 0;
        }
    }

    return count;
}

/* ============================ Search & Eval ============================= */
static void applyMove(GameState *g, const Move *m, Undo *u) {
    /* Save just the state slices that this move can change.
     * Narrow undo snapshots are smaller and faster than cloning GameState. */
    u->oldNeutral = g->neutral;
    u->oldSide = g->side;
    u->oldWinner = g->winner;
    u->oldPly = g->ply;

    /* Hands are side-dependent, so we snapshot the hand of side-to-move only. */
    if (g->side == HUMAN) {
        u->oldHand0 = g->handH[0];
        u->oldHand1 = g->handH[1];
    } else {
        u->oldHand0 = g->handC[0];
        u->oldHand1 = g->handC[1];
    }

    /* Clear stale winner before re-evaluating win condition for this move. */
    g->winner = 0;

    /* Real board movement (discard moves skip this branch by design). */
    if (m->piece != 0) {
        g->board[m->from] = EMPTY;
        g->board[m->to] = m->piece;
        /* Win is checked from the moving side perspective before turn flip. */
        if (isWinningMove(m, g->side)) {
            g->winner = g->side;
        }
    }

    /* Card rotation happens every turn, including forced discard.
     * Keeping this unconditional avoids rule drift between edge cases. */
    if (g->side == HUMAN) {
        unsigned char used = g->handH[m->cardSlot];
        g->handH[m->cardSlot] = g->neutral;
        g->neutral = used;
    } else {
        unsigned char used = g->handC[m->cardSlot];
        g->handC[m->cardSlot] = g->neutral;
        g->neutral = used;
    }

    /* Handled turn: pass move to opponent. */
    g->side = -g->side;
    /* Ply cap uses this counter, so we update it for every completed turn. */
    if (g->ply < 255) ++g->ply;
}

static void undoMove(GameState *g, const Move *m, const Undo *u) {
    /* Restore metadata first: hand restoration below depends on restored side. */
    g->neutral = u->oldNeutral;
    g->side = u->oldSide;
    g->winner = u->oldWinner;
    g->ply = u->oldPly;

    /* Restore the hand that was active before applyMove(). */
    if (g->side == HUMAN) {
        g->handH[0] = u->oldHand0;
        g->handH[1] = u->oldHand1;
    } else {
        g->handC[0] = u->oldHand0;
        g->handC[1] = u->oldHand1;
    }

    /* Restore board only for real piece moves. Discard moves never touched it. */
    if (m->piece != 0) {
        g->board[m->from] = m->piece;
        g->board[m->to] = m->captured;
    }
}

static int moveOrderScore(const Move *m, signed char side) {
    /* This is only for move ordering, not for deciding if a position is good.
     * We sort better tactical moves first to get cutoffs earlier. */
    int score = 0;

    /* Synthetic discard moves are always explored last.
     * They are legal but rarely tactically urgent. */
    if (m->piece == 0) return -600;

    /* If a move wins immediately, we force it to the very top. */
    if (isWinningMove(m, side)) score += 10000;

    /* Captures are usually forcing and useful for pruning.
     * Capturing a master is almost a win and gets a huge priority. */
    if (m->captured != 0) {
        score += (m->captured == H_MASTER || m->captured == C_MASTER) ? 9000 : 250;
    }

    /* Small center bonus: central squares typically keep options open.
     * This is cheap and helps ordering quality with minimal CPU cost. */
    unsigned char tx = m->to % BOARD_N;
    unsigned char ty = m->to / BOARD_N;
    signed char dx = (signed char)tx - 2;
    signed char dy = (signed char)ty - 2;
    score += (8 - (dx < 0 ? -dx : dx) - (dy < 0 ? -dy : dy));

    return score;
}

static int SortScores[MAX_MOVES];

static void sortMoves(const Move *movesIn, unsigned char n, signed char side, Move *movesOut) {
    /* Copy input and compute all ordering scores once.
     * Keeping scores in a side array avoids recomputing inside the inner loop. */
    for (unsigned char i = 0; i < n; ++i) {
        movesOut[i] = movesIn[i];
        SortScores[i] = moveOrderScore(&movesIn[i], side);
    }

    /* Insertion sort is intentional:
     * - n is very small in this game (cheap O(n^2))
     * - tiny code footprint
     * - excellent on nearly-sorted data between similar positions */
    for (unsigned char i = 1; i < n; ++i) {
        Move key = movesOut[i];
        int keyScore = SortScores[i];
        signed char j = (signed char)i - 1;

        /* Shift lower-priority moves right until insertion point is found. */
        while (j >= 0 && SortScores[(unsigned char)j] < keyScore) {
            movesOut[(unsigned char)(j + 1)] = movesOut[(unsigned char)j];
            SortScores[(unsigned char)(j + 1)] = SortScores[(unsigned char)j];
            --j;
        }
        /* Insert current move in its sorted slot. */
        movesOut[(unsigned char)(j + 1)] = key;
        SortScores[(unsigned char)(j + 1)] = keyScore;
    }
}

static int findMasterSq(const GameState *g, signed char side) {
    signed char target = (side == HUMAN) ? H_MASTER : C_MASTER;
    for (unsigned char i = 0; i < BOARD_CELLS; ++i) {
        if (g->board[i] == target) return i;
    }
    return -1;
}

static int manhattanSq(int a, int b) {
    int ax = a % BOARD_N, ay = a / BOARD_N;
    int bx = b % BOARD_N, by = b / BOARD_N;
    int dx = ax - bx;
    int dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

static int evalPosition(const GameState *g) {
    /* Terminal handling first: fast exit and stable score scale. */
    if (g->winner == AI) return 20000 - g->ply;
    if (g->winner == HUMAN) return -20000 + g->ply;
    if (g->ply >= MAX_PLY) return 0;

    /* Score is always in AI-centric perspective. */
    int score = 0;

    /* Material term:
     * master is much more valuable than student because it is both king and
     * temple-race piece. */
    for (unsigned char i = 0; i < BOARD_CELLS; ++i) {
        switch (g->board[i]) {
            case C_STUDENT: score += 120; break;
            case H_STUDENT: score -= 120; break;
            case C_MASTER:  score += 1000; break;
            case H_MASTER:  score -= 1000; break;
            default: break;
        }
    }

    /* Temple race term:
     * if AI master is closer to human temple, we like the position;
     * if human master is closer to AI temple, we dislike it. */
    int aiMasterSq = findMasterSq(g, AI);
    int humanMasterSq = findMasterSq(g, HUMAN);
    if (aiMasterSq >= 0 && humanMasterSq >= 0) {
        int aiTempleDist = manhattanSq(aiMasterSq, idxOf(2, 4));
        int humanTempleDist = manhattanSq(humanMasterSq, idxOf(2, 0));
        score += (humanTempleDist - aiTempleDist) * 12;
    }

    /* We estimate pressure directly here instead of calling full move-gen
     * again for every leaf: this keeps depth stable on slow targets. */
    unsigned char aiMobility = 0, humanMobility = 0;
    unsigned char threatsOnHuman = 0, threatsOnAI = 0;
    unsigned char aiWinsInOne = 0, humanWinsInOne = 0;

    /* Scan every piece and project pseudo-legal destinations from both cards.
     * We only need coarse pressure metrics here, not full move objects. */
    for (unsigned char fromSq = 0; fromSq < BOARD_CELLS; ++fromSq) {
        signed char piece = g->board[fromSq];
        if (piece == EMPTY) continue;

        signed char pieceSide = ownerOfPiece(piece);
        signed char fromX = fromSq % BOARD_N;
        signed char fromY = fromSq / BOARD_N;
        const unsigned char *hand = (pieceSide == HUMAN) ? g->handH : g->handC;

        /* Each side has exactly two cards available this turn. */
        for (unsigned char cs = 0; cs < 2; ++cs) {
            const MoveCard *card = &Cards[hand[cs]];
            /* For each pattern offset in the selected card... */
            for (unsigned char mi = 0; mi < (unsigned char)card->moveCount; ++mi) {
                signed char dx = card->dx[mi];
                signed char dy = card->dy[mi];
                /* Card geometry is mirrored for AI side in board coordinates. */
                if (pieceSide == AI) { dx = -dx; dy = -dy; }

                signed char nx = fromX + dx;
                signed char ny = fromY + dy;
                /* Ignore off-board projections. */
                if (!inBounds(nx, ny)) continue;

                signed char target = g->board[idxOf((unsigned char)nx, (unsigned char)ny)];
                /* Own-piece collisions do not contribute pressure. */
                if (ownerOfPiece(target) == pieceSide) continue;

                if (pieceSide == AI) {
                    /* Mobility = count of reachable legal destinations. */
                    ++aiMobility;
                    /* Direct master threat is highly relevant tactically. */
                    if (target == H_MASTER) ++threatsOnHuman;
                    /* Detect immediate win patterns in one move. */
                    if (target == H_MASTER || (piece == C_MASTER && idxOf((unsigned char)nx, (unsigned char)ny) == idxOf(2, 4)))
                        ++aiWinsInOne;
                } else {
                    ++humanMobility;
                    if (target == C_MASTER) ++threatsOnAI;
                    if (target == C_MASTER || (piece == H_MASTER && idxOf((unsigned char)nx, (unsigned char)ny) == idxOf(2, 0)))
                        ++humanWinsInOne;
                }
            }
        }
    }

    /* Combine pressure terms with tuned weights.
     * Win-in-one dominates threat dominates generic mobility. */
    score += ((int)aiMobility - (int)humanMobility) * 4;
    score += ((int)threatsOnHuman - (int)threatsOnAI) * 85;
    score += ((int)aiWinsInOne - (int)humanWinsInOne) * 420;

    return score;
}

static int evalForSide(const GameState *g, signed char side) {
    /* Keep a single evaluator (AI-centric) and flip sign when needed.
     * This keeps negamax symmetric and avoids duplicated eval code. */
    int aiScore = evalPosition(g);
    return (side == AI) ? aiScore : -aiScore;
}

static int negamax(GameState *g, int depth, int alpha, int beta) {
    /* Stop on:
     * - depth horizon
     * - terminal game state
     * - forced draw by ply cap */
    if (depth <= 0 || g->winner != 0 || g->ply >= MAX_PLY) {
        return evalForSide(g, g->side);
    }

    /* Each depth uses its dedicated scratch layer so recursion does not
     * allocate move arrays on stack repeatedly. */
    unsigned char layer = (unsigned char)depth;
    Move *rawMoves = S.searchRaw[layer];
    Move *moves = S.searchSorted[layer];

    /* Generate legal moves for side-to-move in this node. */
    unsigned char moveCount = generateMoves(g, g->side, rawMoves);

    /* Order moves before search to maximize alpha-beta cutoffs. */
    sortMoves(rawMoves, moveCount, g->side, moves);

    /* Best score seen in this node so far. */
    int best = -INF_SCORE;

    /* Explore children in sorted order. */
    for (unsigned char i = 0; i < moveCount; ++i) {
        /* Yield periodically so render/input keep breathing while AI searches. */
        searchHeartbeat();

        /* Make move on board state. */
        Undo undo;
        applyMove(g, &moves[i], &undo);

        /* Negamax identity:
         * score(node) = -score(child) with flipped alpha/beta window. */
        int score = -negamax(g, depth - 1, -beta, -alpha);

        /* Restore board exactly as it was before exploring next sibling. */
        undoMove(g, &moves[i], &undo);

        /* Track node best. */
        if (score > best) best = score;
        /* Raise alpha (lower bound) when we found a better line. */
        if (score > alpha) alpha = score;
        /* Standard alpha-beta cutoff: once this branch is proven too good for
         * the side to move, siblings cannot change the parent decision. */
        if (alpha >= beta) break;
    }

    return best;
}

static void cardShortName(unsigned char cardIdx, char out5[6]) {
    const char *namePtr = Cards[cardIdx].name;
    unsigned char written = 0;
    while (*namePtr && written < 5) {
        char ch = *namePtr++;
        if (ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - ('a' - 'A'));
        out5[written++] = ch;
    }
    while (written < 5) out5[written++] = ' ';
    out5[5] = '\0';
}

static void drawCard(unsigned char sx, unsigned char sy, unsigned char cardIdx, signed char ownerSide, bool selected, bool neutral) {
    const MoveCard *card = &Cards[cardIdx];
    unsigned char titleColor = neutral ? VCOL_WHITE : (ownerSide == HUMAN ? VCOL_RED : VCOL_LT_BLUE);
    unsigned char dotColor = VCOL_DARK_GREY;
    unsigned char moveColor = neutral ? VCOL_LT_GREY : (ownerSide == HUMAN ? VCOL_RED : VCOL_LT_BLUE);
    if (selected) moveColor = VCOL_YELLOW;

    char name5[6];
    cardShortName(cardIdx, name5);

    if (selected) {
        if (sx > 0) screenPut((unsigned char)(sx - 1), sy, '[', VCOL_YELLOW);
        screenPut((unsigned char)(sx + 5), sy, ']', VCOL_YELLOW);
    }
    drawText(sx, sy, name5, selected ? VCOL_WHITE : titleColor);

    for (unsigned char gy = 0; gy < 5; ++gy) {
        for (unsigned char gx = 0; gx < 5; ++gx) {
            bool isOrigin = (gx == 2 && gy == 2);
            bool isMove = false;
            unsigned char moveColorLocal = moveColor;

            for (unsigned char i = 0; i < (unsigned char)card->moveCount; ++i) {
                signed char dx = card->dx[i], dy = card->dy[i];
                if (ownerSide == AI) {
                    dx = -dx;
                    dy = -dy;
                }

                signed char mx = (signed char)(2 + dx);
                signed char my = (signed char)(2 + dy);
                if (mx == (signed char)gx && my == (signed char)gy) {
                    isMove = true;
                }
            }

            if (isMove) {
                screenPut((unsigned char)(sx + gx), (unsigned char)(sy + 1 + gy), '*', moveColorLocal);
            } else if (isOrigin) {
                screenPutCode((unsigned char)(sx + gx), (unsigned char)(sy + 1 + gy), CH_CARD_ORIGIN, VCOL_WHITE);
            } else {
                screenPut((unsigned char)(sx + gx), (unsigned char)(sy + 1 + gy), '.', dotColor);
            }
        }
    }
}

/* ================================ UI ==================================== */
static void drawBoard(const GameState *g, const HumanUI *ui) {
    /* Alphanumeric coordinates reduce ambiguity when players call moves or
     * compare behavior with the physical board game notation. */
    drawText((unsigned char)(GRID_X + 2), (unsigned char)(GRID_Y - 1), "A   B   C   D   E", VCOL_WHITE);

    /* Rebuild the board every frame: on C64 this is simpler and safer than
     * dirty rectangles, and avoids stale artifacts after rapid state changes. */
    for (unsigned char gy = 0; gy <= BOARD_N; ++gy) {
        unsigned char yline = (unsigned char)(GRID_Y + gy * CELL_H);
        for (unsigned char gx = 0; gx <= (unsigned char)(BOARD_N * CELL_W); ++gx) {
            unsigned char x = (unsigned char)(GRID_X + gx);
            char ch = (gx % CELL_W == 0) ? '+' : '-';
            screenPut(x, yline, ch, VCOL_BLUE);
        }
    }

    for (unsigned char gy = 0; gy < BOARD_N; ++gy) {
        for (unsigned char yy = 1; yy < CELL_H; ++yy) {
            unsigned char y = (unsigned char)(GRID_Y + gy * CELL_H + yy);
            for (unsigned char gx = 0; gx <= (unsigned char)(BOARD_N * CELL_W); ++gx) {
                unsigned char x = (unsigned char)(GRID_X + gx);
                if (gx % CELL_W == 0) {
                    screenPut(x, y, '|', VCOL_BLUE);
                } else {
                    screenPut(x, y, ' ', VCOL_BLACK);
                }
            }
        }
    }

    /* Numbering from 5 to 1 matches top-to-bottom board reading. */
    for (unsigned char y = 0; y < BOARD_N; ++y) {
        screenPut((unsigned char)(GRID_X - 2), cellCenterY(y), (char)('5' - y), VCOL_WHITE);
    }

    /* Dot-only empty cells avoid confusion with piece glyph leftovers and make
     * any misplaced draw immediately obvious while debugging. */
    for (unsigned char y = 0; y < BOARD_N; ++y) {
        for (unsigned char x = 0; x < BOARD_N; ++x) {
            char ch = '.';
            unsigned char col = VCOL_DARK_GREY;

            screenPut(cellCenterX(x), cellCenterY(y), ch, col);
        }
    }

    /* Highlights must be louder than piece art, otherwise cursor movement
     * feels lost when using joystick-only navigation. */
    if (ui) {
        for (unsigned char y = 0; y < BOARD_N; ++y) {
            for (unsigned char x = 0; x < BOARD_N; ++x) {
                unsigned char sq = idxOf(x, y);
                if (isMarkedDestination(ui, sq)) {
                    unsigned char x0 = (unsigned char)(GRID_X + x * CELL_W);
                    unsigned char y0 = (unsigned char)(GRID_Y + y * CELL_H);
                    unsigned char x1 = (unsigned char)(x0 + CELL_W);
                    unsigned char y1 = (unsigned char)(y0 + CELL_H);

                    for (unsigned char xx = (unsigned char)(x0 + 1); xx < x1; ++xx) {
                        screenPut(xx, y0, '-', VCOL_YELLOW);
                        screenPut(xx, y1, '-', VCOL_YELLOW);
                    }
                    for (unsigned char yy = (unsigned char)(y0 + 1); yy < y1; ++yy) {
                        screenPut(x0, yy, '|', VCOL_YELLOW);
                        screenPut(x1, yy, '|', VCOL_YELLOW);
                    }
                    screenPut(x0, y0, '+', VCOL_YELLOW);
                    screenPut(x1, y0, '+', VCOL_YELLOW);
                    screenPut(x0, y1, '+', VCOL_YELLOW);
                    screenPut(x1, y1, '+', VCOL_YELLOW);
                }
            }
        }

        if (ui->selectedFrom != 255) {
            unsigned char sx = (unsigned char)(ui->selectedFrom % BOARD_N);
            unsigned char sy = (unsigned char)(ui->selectedFrom / BOARD_N);
            unsigned char x0 = (unsigned char)(GRID_X + sx * CELL_W);
            unsigned char y0 = (unsigned char)(GRID_Y + sy * CELL_H);
            unsigned char x1 = (unsigned char)(x0 + CELL_W);
            unsigned char y1 = (unsigned char)(y0 + CELL_H);

            for (unsigned char xx = (unsigned char)(x0 + 1); xx < x1; ++xx) {
                screenPut(xx, y0, '-', VCOL_WHITE);
                screenPut(xx, y1, '-', VCOL_WHITE);
            }
            for (unsigned char yy = (unsigned char)(y0 + 1); yy < y1; ++yy) {
                screenPut(x0, yy, '|', VCOL_WHITE);
                screenPut(x1, yy, '|', VCOL_WHITE);
            }
            screenPut(x0, y0, '+', VCOL_WHITE);
            screenPut(x1, y0, '+', VCOL_WHITE);
            screenPut(x0, y1, '+', VCOL_WHITE);
            screenPut(x1, y1, '+', VCOL_WHITE);
        }

        if (ui->showCursor) {
            unsigned char x0 = (unsigned char)(GRID_X + ui->cursorX * CELL_W);
            unsigned char y0 = (unsigned char)(GRID_Y + ui->cursorY * CELL_H);
            unsigned char x1 = (unsigned char)(x0 + CELL_W);
            unsigned char y1 = (unsigned char)(y0 + CELL_H);
            unsigned char ccol = VCOL_YELLOW;

            /* Filling the current cell gives constant spatial feedback even on
             * CRT-like blur where border-only cursors are easy to miss. */
            for (unsigned char yy = (unsigned char)(y0 + 1); yy < y1; ++yy) {
                for (unsigned char xx = (unsigned char)(x0 + 1); xx < x1; ++xx) {
                    screenPut(xx, yy, '.', VCOL_YELLOW);
                }
            }

            for (unsigned char xx = (unsigned char)(x0 + 1); xx < x1; ++xx) {
                screenPut(xx, y0, '-', ccol);
                screenPut(xx, y1, '-', ccol);
            }
            for (unsigned char yy = (unsigned char)(y0 + 1); yy < y1; ++yy) {
                screenPut(x0, yy, '|', ccol);
                screenPut(x1, yy, '|', ccol);
            }
            screenPut(x0, y0, '+', ccol);
            screenPut(x1, y0, '+', ccol);
            screenPut(x0, y1, '+', ccol);
            screenPut(x1, y1, '+', ccol);
            screenPut((unsigned char)(x0 + 1), (unsigned char)(y0 + 1), '+', ccol);
        }
    }

    /* Pieces are last so selection overlays never erase tactical information. */
    drawPieces(g);
}

static void drawHud(const GameState *g, const HumanUI *ui, const char *status) {
    unsigned char titleColor = VCOL_WHITE;
    unsigned char selectedCardSlot = ui ? ui->selectedCardSlot : 255;

    for (unsigned char y = 0; y < 25; ++y) {
        clearRow(y, VCOL_BLACK);
    }

    drawText(2, 0, "RONINO C64", titleColor);
    drawText(24, 0, "CPU", VCOL_LT_BLUE);
    drawText(30, 8, "N", VCOL_WHITE);
    drawText(24, 16, "YOU", VCOL_RED);
    drawCard(24, 1, g->handC[0], AI, false, false);
    drawCard(31, 1, g->handC[1], AI, false, false);
    drawCard(31, 9, g->neutral, 0, false, true);
    drawCard(24, 17, g->handH[0], HUMAN, selectedCardSlot == 0, false);
    drawCard(31, 17, g->handH[1], HUMAN, selectedCardSlot == 1, false);

    drawBoard(g, ui);

    drawText(24, 6, "TURN:", VCOL_LT_GREY);
    drawText(30, 6, g->side == HUMAN ? "RED" : "BLUE", g->side == HUMAN ? VCOL_RED : VCOL_LT_BLUE);

    drawText(24, 7, "PIECES:", VCOL_LT_GREY);
    char hudBuf[20];
    sprintf(hudBuf, "R:%u B:%u", (unsigned)countPieces(g, HUMAN), (unsigned)countPieces(g, AI));
    drawText(31, 7, hudBuf, VCOL_WHITE);

    drawText(24, 8, "PLY:", VCOL_LT_GREY);
    sprintf(hudBuf, "%u", (unsigned)g->ply);
    drawText(29, 8, hudBuf, VCOL_WHITE);

    if (ui && ui->showCursor) {
        char cursorBuf[8];
        cursorBuf[0] = 'C'; cursorBuf[1] = 'U'; cursorBuf[2] = 'R'; cursorBuf[3] = ':';
        cursorBuf[4] = (char)('A' + ui->cursorX);
        cursorBuf[5] = (char)('5' - ui->cursorY);
        cursorBuf[6] = '\0';
        drawText(24, 9, cursorBuf, VCOL_YELLOW);
    }

    drawText(1, 22, "MOVE: CURSORS/WASD/JOY   SELECT: FIRE/SPACE", VCOL_LT_GREY);
    drawText(1, 23, "1/2 CARD, X CANCEL, Q QUIT", VCOL_LT_GREY);
    drawText(1, 24, status, VCOL_WHITE);

    /* Intentional no-op: pieces are part of board pass to keep draw order
     * deterministic and avoid split ownership between HUD and board code. */
}

static void drawStatusOnly(const char *status) {
    /* Touch only the status row while CPU thinks so the rest of the screen
     * remains stable and we avoid unnecessary redraw cost. */
    clearRow(24, VCOL_BLACK);
    drawText(1, 24, status, VCOL_WHITE);
}

static bool hasRealMoves(const Move *moves, unsigned char n) {
    for (unsigned char i = 0; i < n; ++i) {
        if (moves[i].piece != 0) return true;
    }
    return false;
}

static bool isHumanPieceSquare(const GameState *g, unsigned char sq) {
    return ownerOfPiece(g->board[sq]) == HUMAN;
}

static unsigned char collectMovesFromSquare(
    const Move *moves,
    unsigned char n,
    unsigned char from,
    unsigned char cardSlotFilter,
    Move *outPieceMoves
) {
    unsigned char moveCount = 0;
    for (unsigned char i = 0; i < n; ++i) {
        if (moves[i].piece != 0 &&
            moves[i].from == from &&
            (cardSlotFilter == 255 || moves[i].cardSlot == cardSlotFilter)) {
            outPieceMoves[moveCount++] = moves[i];
        }
    }
    return moveCount;
}

static unsigned char refreshSelectedMoves(
    const Move *allMoves,
    unsigned char allCount,
    unsigned char from,
    unsigned char *selectedCardSlot,
    Move *outPieceMoves
) {
    /* No card selected yet: no legal targets to preview. */
    if (*selectedCardSlot > 1) return 0;
    return collectMovesFromSquare(allMoves, allCount, from, *selectedCardSlot, outPieceMoves);
}

static bool findMoveToSquare(
    const Move *pieceMoves,
    unsigned char pieceMoveCount,
    unsigned char to,
    Move *outMove
) {
    for (unsigned char i = 0; i < pieceMoveCount; ++i) {
        if (pieceMoves[i].to == to) {
            *outMove = pieceMoves[i];
            return true;
        }
    }
    return false;
}

static int humanChooseMove(GameState *g, Move *outMove) {
    /* This is intentionally a small state machine (select piece -> select card
     * -> select target). Keeping it linear makes joystick and keyboard paths
     * behave exactly the same. */
    Move allMoves[MAX_MOVES];
    Move pieceMoves[8];
    unsigned char allCount = generateMoves(g, HUMAN, allMoves);

    /* This is a real turn, not a skip: the player must still choose
     * which card to rotate. Handling it explicitly avoids hidden rules. */
    if (!hasRealMoves(allMoves, allCount)) {
        for (;;) {
            HumanUI ui = {true, S.cursorX, S.cursorY, 255, 255, 0, 0};
            drawHud(g, &ui, "NO MOVES: PRESS 1 OR 2 TO DISCARD CARD");

            while (!kbhit()) {
                frameTick();
            }
            char key = getch();
            if (key == 'q' || key == 'Q') return 0;
            if (key == '1') { *outMove = allMoves[0]; return 1; }
            if (key == '2') { *outMove = allMoves[1]; return 1; }
        }
    }

    unsigned char selectedFrom = 255;
    unsigned char selectedCardSlot = 255;
    unsigned char pieceMoveCount = 0;

    signed char prevJoyX = 0;
    signed char prevJoyY = 0;
    unsigned char prevJoyButton = 0;
    unsigned char fireArmed = 0;

    /* If FIRE is still held from title/previous action, do not treat it as a
     * fresh press for the first input frame of this turn. */
    joy_poll(JOY_PORT);
    prevJoyX = joyx[JOY_PORT];
    prevJoyY = joyy[JOY_PORT];
    prevJoyButton = joyb[JOY_PORT];
    fireArmed = (prevJoyButton == 0);
    /* Drop stale keypresses from title/previous turn. */
    while (kbhit()) (void)getch();

    for (;;) {
        HumanUI ui;
        ui.showCursor = true;
        ui.cursorX = S.cursorX;
        ui.cursorY = S.cursorY;
        ui.selectedFrom = selectedFrom;
        ui.selectedCardSlot = selectedCardSlot;
        ui.movesFromSelected = pieceMoves;
        ui.movesFromSelectedCount = pieceMoveCount;
        drawHud(g, &ui, selectedFrom == 255
            ? "SELECT RED PIECE. THEN SELECT CARD 1/2 AND TARGET."
            : "CARD 1/2 TOGGLE, YELLOW CELLS = LEGAL TARGETS");

        char action = 0;
        while (!action) {
            frameTick();

            /* Edge-trigger joystick input: if we react to held directions, the
             * cursor skips cells too fast to read on a 5x5 board. */
            joy_poll(JOY_PORT);
            if (!joyb[JOY_PORT]) fireArmed = 1;
            if (joyx[JOY_PORT] < 0 && prevJoyX >= 0) action = 'L';
            else if (joyx[JOY_PORT] > 0 && prevJoyX <= 0) action = 'R';
            else if (joyy[JOY_PORT] < 0 && prevJoyY >= 0) action = 'U';
            else if (joyy[JOY_PORT] > 0 && prevJoyY <= 0) action = 'D';
            else if (joyb[JOY_PORT] && !prevJoyButton && fireArmed) action = 'F';
            prevJoyX = joyx[JOY_PORT];
            prevJoyY = joyy[JOY_PORT];
            prevJoyButton = joyb[JOY_PORT];

            if (kbhit()) {
                char key = getch();
                if (key >= 'a' && key <= 'z') key = (char)(key - ('a' - 'A'));
                if (key == 'A') action = 'L';
                else if (key == 'D') action = 'R';
                else if (key == 'W') action = 'U';
                else if (key == 'S') action = 'D';
                else if (key == PETSCII_CURSOR_LEFT) action = 'L';
                else if (key == PETSCII_CURSOR_RIGHT) action = 'R';
                else if (key == PETSCII_CURSOR_UP) action = 'U';
                else if (key == PETSCII_CURSOR_DOWN) action = 'D';
                else if (key == ' ' || key == '\r' || key == '\n') action = 'F';
                else if (key == '1') action = '1';
                else if (key == '2') action = '2';
                else if (key == 'X') action = 'C';
                else if (key == 'Q') action = 'Q';
            }
        }

        if (action == 'Q') return 0;
        if (action == 'L' && S.cursorX > 0) --S.cursorX;
        if (action == 'R' && S.cursorX < BOARD_N - 1) ++S.cursorX;
        if (action == 'U' && S.cursorY > 0) --S.cursorY;
        if (action == 'D' && S.cursorY < BOARD_N - 1) ++S.cursorY;
        if (action == '1' || action == '2') {
            /* Let user pre-select card before piece: many players think
             * "card first, piece second", so preview must honor that flow. */
            selectedCardSlot = (action == '1') ? 0 : 1;
            if (selectedFrom != 255) {
                pieceMoveCount = refreshSelectedMoves(allMoves, allCount, selectedFrom, &selectedCardSlot, pieceMoves);
            } else {
                pieceMoveCount = 0;
            }
            continue;
        }
        if (action == 'C') {
            selectedFrom = 255;
            selectedCardSlot = 255;
            pieceMoveCount = 0;
            continue;
        }
        if (action != 'F') continue;

        unsigned char cursorSq = idxOf(S.cursorX, S.cursorY);

        if (selectedFrom == 255) {
            if (!isHumanPieceSquare(g, cursorSq)) continue;
            selectedFrom = cursorSq;
            /* If a card was pre-selected, show destinations immediately;
             * otherwise keep state cardless and wait for 1/2 input. */
            if (selectedCardSlot <= 1) {
                pieceMoveCount = refreshSelectedMoves(allMoves, allCount, selectedFrom, &selectedCardSlot, pieceMoves);
            } else {
                pieceMoveCount = 0;
            }
            continue;
        }

        if (cursorSq == selectedFrom) {
            selectedFrom = 255;
            selectedCardSlot = 255;
            pieceMoveCount = 0;
            continue;
        }

        if (isHumanPieceSquare(g, cursorSq)) {
            selectedFrom = cursorSq;
            pieceMoveCount = refreshSelectedMoves(allMoves, allCount, selectedFrom, &selectedCardSlot, pieceMoves);
            continue;
        }

        if (findMoveToSquare(pieceMoves, pieceMoveCount, cursorSq, outMove)) {
            return 1;
        }
    }
}

static void aiThinkBegin(const GameState *g) {
    /* Prepare root moves once, then evaluate one per frame in aiThinkStep().
     * This keeps the machine responsive without giving up full depth search. */
    /* We search from a copy of game state so thinking never mutates live state
     * until the final best move is committed by main loop. */
    unsigned char layer = SEARCH_DEPTH;
    Move *rawMoves = S.searchRaw[layer];
    Move *sortedMoves = S.searchSorted[layer];

    /* Reset whole AI turn state in one shot. */
    memset(&S.ai, 0, sizeof(S.ai));
    S.ai.active = true;
    S.ai.probe = *g;

    /* Build and order root candidates exactly once for this turn. */
    S.ai.rootCount = generateMoves(&S.ai.probe, AI, rawMoves);
    sortMoves(rawMoves, S.ai.rootCount, AI, sortedMoves);
    for (unsigned char i = 0; i < S.ai.rootCount; ++i) {
        S.ai.rootMoves[i] = sortedMoves[i];
    }

    /* Seed best move with first candidate so we always have a valid fallback
     * even if all lines evaluate equally. */
    S.ai.bestMove = S.ai.rootMoves[0];
    S.ai.bestScore = -INF_SCORE;
    S.ai.rootIndex = 0;
    S.searchPulse = 0;
}

static bool aiThinkStep(Move *outBest, int *outScore) {
    /* No active search means caller asked too early or search already ended. */
    if (!S.ai.active || S.ai.rootCount == 0) return false;

    /* Evaluate exactly one root move per call to keep frame pacing stable. */
    if (S.ai.rootIndex < S.ai.rootCount) {
        /* Root-level incremental search trades a small amount of throughput for
         * stable rendering, which is a much better UX on this hardware. */
        Undo undo;
        Move *candidate = &S.ai.rootMoves[S.ai.rootIndex];

        /* Apply candidate on probe state, search reply tree, then undo. */
        applyMove(&S.ai.probe, candidate, &undo);
        int score = -negamax(&S.ai.probe, SEARCH_DEPTH - 1, -INF_SCORE, INF_SCORE);
        undoMove(&S.ai.probe, candidate, &undo);

        /* Update best-so-far root move. */
        if (S.ai.rootIndex == 0 || score > S.ai.bestScore) {
            S.ai.bestScore = score;
            S.ai.bestMove = *candidate;
        }

        /* Advance to next root candidate for next frame/call. */
        ++S.ai.rootIndex;
    }

    /* When all root moves are done, publish final decision. */
    if (S.ai.rootIndex >= S.ai.rootCount) {
        S.ai.active = false;
        *outBest = S.ai.bestMove;
        if (outScore) *outScore = S.ai.bestScore;
        return true;
    }
    return false;
}

static void showGameOver(GameState *g) {
    /* Reuse the normal HUD to avoid maintaining a separate "end screen"
     * rendering path with duplicated layout logic. */
    char gameOverLine[40];

    if (g->winner == HUMAN) {
        strcpy(gameOverLine, "YOU WIN! PRESS N=NEW GAME, Q=QUIT");
    } else if (g->winner == AI) {
        strcpy(gameOverLine, "CPU WINS! PRESS N=NEW GAME, Q=QUIT");
    } else {
        strcpy(gameOverLine, "DRAW (PLY LIMIT). N=NEW GAME, Q=QUIT");
    }

    drawHud(g, 0, gameOverLine);

    for (;;) {
        while (!kbhit()) {
            frameTick();
        }
        char key = getch();
        if (key == 'q' || key == 'Q') {
            S.running = false;
            return;
        }
        if (key == 'n' || key == 'N') {
            newGame(g);
            return;
        }
    }
}

static void maybeDeclareDraw(GameState *g) {
    /* Hard ply cap prevents endless tactical loops and guarantees every match
     * eventually terminates even with imperfect evaluation. */
    if (g->winner == 0 && g->ply >= MAX_PLY) {
        g->winner = DRAW;
    }
}

static void titleWaitAndSeed(void) {
    vic.color_border = VCOL_BLACK;
    vic.color_back = VCOL_BLACK;
    charsetInit();

    clrscr();
    drawText(9, 8, "RONINO C64", VCOL_WHITE);
    drawText(3, 10, "SINGLE PLAYER: HUMAN (RED) VS CPU (BLUE)", VCOL_LT_GREY);
    drawText(12, 12, "FAN STRATEGY DUEL", VCOL_MED_GREY);
    drawText(4, 14, "PRESS ANY KEY OR JOY FIRE", VCOL_WHITE);

    unsigned int seed = 0;
    while (1) {
        /* Raster timing is cheap entropy on C64; it is enough to avoid opening
         * repetition without introducing platform-specific RNG code. */
        seed += (unsigned int)(vic.raster + 1);
        frameTick();

        joy_poll(JOY_PORT);
        if (joyb[JOY_PORT]) {
            break;
        }

        if (kbhit()) {
            (void)getch();
            break;
        }
    }

    if (seed == 0) seed = 1;
    srand(seed);

    /* If title was dismissed with joystick FIRE held, wait release now so
     * the first in-game selection press is always edge-clean and intentional. */
    do {
        frameTick();
        joy_poll(JOY_PORT);
    } while (joyb[JOY_PORT]);
}

int main(void) {
    /* Main loop is turn-driven and intentionally explicit: each branch does
     * input/search, applies one move, then redraws. This avoids hidden state. */
    titleWaitAndSeed();
    newGame(&S.game);

    while (S.running) {
        maybeDeclareDraw(&S.game);

        if (S.game.winner != 0) {
            showGameOver(&S.game);
            continue;
        }

        if (S.game.side == HUMAN) {
            S.ai.active = false;
            Move humanMove;
            if (!humanChooseMove(&S.game, &humanMove)) {
                S.running = false;
                break;
            }

            char statusLine[40];
            strcpy(statusLine, "YOU MOVED");

            Undo undo;
            applyMove(&S.game, &humanMove, &undo);

            drawHud(&S.game, 0, statusLine);
            for (unsigned char i = 0; i < 12; ++i) frameTick();
        } else {
            if (!S.ai.active) {
                aiThinkBegin(&S.game);
                drawHud(&S.game, 0, "CPU THINKING...");
            }

            Move aiMove;
            int score = 0;
            if (aiThinkStep(&aiMove, &score)) {
                char statusLine[40];
                sprintf(statusLine, "CPU MOVED (%d)", score);

                Undo undo;
                applyMove(&S.game, &aiMove, &undo);

                drawHud(&S.game, 0, statusLine);
                for (unsigned char i = 0; i < 30; ++i) frameTick();
            } else {
                char thinkStatus[40];
                sprintf(thinkStatus, "CPU THINKING %u/%u", (unsigned)(S.ai.rootIndex + 1), (unsigned)S.ai.rootCount);
                drawStatusOnly(thinkStatus);
                frameTick();
            }
        }
    }

    clrscr();
    drawText(10, 12, "Bye!", VCOL_WHITE);

    return 0;
}
