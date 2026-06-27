// Numberator — the calculator itself: a small immediate-execution state machine over a typed entry and
// one pending operator. Pure logic, no engine types — the renderer only ever asks for display().
#pragma once

#include <string>

#include "layout.h"  // Action

namespace numberator {

class Calculator {
public:
    void press(Action action, char data);   // data: a digit for Digit, an operator char for Op
    [[nodiscard]] std::string display() const;  // the string the well shows (right-aligned by the view)

private:
    void inputDigit(char d);
    void inputDecimal();
    void setOperator(char op);
    void equals();
    void clear();
    void negate();
    void percent();
    void backspace();
    [[nodiscard]] double current() const;  // the entry parsed to a number
    void showResult(double value);         // format a result back into the entry

    std::string entry_ = "0";  // the digits currently shown
    double      acc_   = 0.0;  // the accumulated left operand
    char        op_    = 0;    // the pending operator, or 0
    bool        fresh_ = true; // the next digit starts a new entry (replaces it)
    bool        error_ = false;
};

}  // namespace numberator
