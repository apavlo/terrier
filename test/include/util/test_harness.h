#pragma once

#include <memory>
#include "gtest/gtest.h"
#include "loggers/catalog_logger.h"
#include "loggers/index_logger.h"
#include "loggers/main_logger.h"
#include "loggers/storage_logger.h"
#include "loggers/transaction_logger.h"

namespace terrier {

class TerrierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // initialize loggers

    init_main_logger();
    // initialize namespace specific loggers
    terrier::storage::init_index_logger();
    terrier::storage::init_storage_logger();
    terrier::transaction::init_transaction_logger();
    terrier::catalog::init_catalog_logger();
  }

  void TearDown() override {
    // shutdown loggers
    spdlog::shutdown();
  }
};

}  // namespace terrier
