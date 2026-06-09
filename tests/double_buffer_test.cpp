#include <gtest/gtest.h>

#include "gbcpp/double_buffer.h"

using gbcpp::DoubleBuffer;

TEST(DoubleBuffer, CurrentAndPreviousAreIndependent) {
    DoubleBuffer<int> db;
    db.current() = 7;
    EXPECT_EQ(db.current(), 7);
    EXPECT_EQ(db.previous(), 0);  // value-initialised, untouched by writing current()
}

TEST(DoubleBuffer, AdvanceCopiesCurrentIntoPrevious) {
    DoubleBuffer<int> db;
    db.current() = 42;
    db.advance();
    EXPECT_EQ(db.previous(), 42);
    EXPECT_EQ(db.current(), 42);
}

TEST(DoubleBuffer, MutatingCurrentAfterAdvanceLeavesPreviousUntouched) {
    DoubleBuffer<int> db;
    db.current() = 1;
    db.advance();      // previous = 1
    db.current() = 2;  // current moves on
    EXPECT_EQ(db.previous(), 1);
    EXPECT_EQ(db.current(), 2);
}
