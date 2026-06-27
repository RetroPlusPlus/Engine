#include "calculator.h"

#include <cmath>
#include <cstdio>

namespace numberator {

namespace {

constexpr int kMaxDigits = 9;  // how many digits an entry accepts before it stops growing

// Format a result for the display: compact, finite-checked. Non-finite (a divide-by-zero) reports the
// error sentinel the caller turns into "E".
std::string formatNumber(double v) {
    if (!std::isfinite(v)) return "";  // empty -> the caller raises the error flag
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.8g", v);
    std::string s = buf;
    // The display font only carries digits, '.', and '-'. A result that needs scientific notation or
    // overruns the well is an overflow — report it as an error (empty) so it shows the error dashes.
    if (s.find('e') != std::string::npos || s.find('E') != std::string::npos || s.size() > 10) return "";
    return s;
}

double applyOp(double a, char op, double b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b == 0.0 ? std::nan("") : a / b;
        default:  return b;
    }
}

bool hasDot(const std::string& s) { return s.find('.') != std::string::npos; }

}  // namespace

void Calculator::press(Action action, char data) {
    if (error_ && action != Action::Clear) return;  // after an error only Clear recovers
    switch (action) {
        case Action::Digit:     inputDigit(data); break;
        case Action::Decimal:   inputDecimal();   break;
        case Action::Op:        setOperator(data); break;
        case Action::Equals:    equals();   break;
        case Action::Clear:     clear();    break;
        case Action::Negate:    negate();   break;
        case Action::Percent:   percent();  break;
        case Action::Backspace: backspace();break;
    }
}

std::string Calculator::display() const { return error_ ? "--------" : entry_; }

double Calculator::current() const {
    try {
        return std::stod(entry_);
    } catch (...) {
        return 0.0;
    }
}

void Calculator::showResult(double value) {
    const std::string s = formatNumber(value);
    if (s.empty()) {
        error_ = true;
        entry_ = "0";
    } else {
        entry_ = s;
    }
    fresh_ = true;
}

void Calculator::inputDigit(char d) {
    if (fresh_) {
        entry_ = std::string(1, d);
        fresh_ = false;
        return;
    }
    if (entry_ == "0")  entry_ = std::string(1, d);
    else if (entry_ == "-0") entry_ = "-" + std::string(1, d);
    else if (static_cast<int>(entry_.size()) < kMaxDigits + (entry_[0] == '-' ? 1 : 0) + (hasDot(entry_) ? 1 : 0))
        entry_ += d;
}

void Calculator::inputDecimal() {
    if (fresh_) {
        entry_ = "0.";
        fresh_ = false;
        return;
    }
    if (!hasDot(entry_)) entry_ += '.';
}

void Calculator::setOperator(char op) {
    if (op_ != 0 && !fresh_) {
        acc_ = applyOp(acc_, op_, current());
        showResult(acc_);
        if (error_) return;
        acc_ = current();
    } else {
        acc_ = current();
    }
    op_    = op;
    fresh_ = true;
}

void Calculator::equals() {
    if (op_ == 0) return;
    const double result = applyOp(acc_, op_, current());
    showResult(result);
    acc_ = error_ ? 0.0 : current();
    op_  = 0;
}

void Calculator::clear() {
    entry_ = "0";
    acc_   = 0.0;
    op_    = 0;
    fresh_ = true;
    error_ = false;
}

void Calculator::negate() {
    if (entry_ == "0" || entry_ == "0.") return;
    if (!entry_.empty() && entry_[0] == '-') entry_.erase(entry_.begin());
    else entry_ = "-" + entry_;
}

void Calculator::percent() { showResult(current() / 100.0); }

void Calculator::backspace() {
    if (fresh_) return;
    if (entry_.size() <= 1 || (entry_.size() == 2 && entry_[0] == '-')) {
        entry_ = "0";
        fresh_ = true;
        return;
    }
    entry_.pop_back();
}

}  // namespace numberator
