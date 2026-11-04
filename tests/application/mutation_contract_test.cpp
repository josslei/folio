#include <folio/application/mutation/mutation_result.hpp>

#include <gtest/gtest.h>

#include <type_traits>
#include <variant>

namespace {
namespace app = folio::application;
namespace core = folio::core;

static_assert(!std::is_convertible_v<core::Id, app::MutationId>);
static_assert(!std::is_convertible_v<app::MutationId, core::Id>);
static_assert(!std::is_constructible_v<app::MutationId, core::Id>);

TEST(MutationIdentity, RoundTripsCanonicalUuidWithoutBecomingAnEntryId) {
  const auto id = app::MutationId::generate();
  const auto text = id.to_string();
  const auto parsed = app::MutationId::parse(text);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, id);
  ASSERT_EQ(text.size(), 36u);
  EXPECT_EQ(text[14], '4');
  EXPECT_NE(std::string{"89ab"}.find(text[19]), std::string::npos);
}

TEST(MutationIdentity, RejectsMalformedAndNonVersionFourIdentifiers) {
  EXPECT_FALSE(app::MutationId::parse(""));
  EXPECT_FALSE(app::MutationId::parse("not-a-mutation-id"));
  EXPECT_FALSE(app::MutationId::parse("00000000-0000-0000-0000-000000000000"));
  EXPECT_FALSE(app::MutationId::parse("550e8400-e29b-11d4-a716-446655440000"));
  EXPECT_FALSE(app::MutationId::parse("550e8400-e29b-41d4-a716-446655440000extra"));
}

TEST(MutationContract, AppliedFailureStillCarriesConfirmedPersistenceEvidence) {
  const auto mutation_id = app::MutationId::generate();
  const auto entry_id = core::Id::generate();
  app::MutationResult<core::Id> result = app::MutationResponse<core::Id>{
      mutation_id,
      app::MutationFailure{
          {app::ErrorCategory::InternalFailure, app::ErrorReason::UnexpectedFailure,
           "Metadata synchronization failed", {app::ErrorAction::Dismiss}},
          app::AuthoritativeChangeStatus::Applied,
          {entry_id, std::nullopt, std::nullopt,
           {{app::MutationStep::WriteEntry, app::StepObservation::DurabilityConfirmed}}}}};

  ASSERT_TRUE(std::holds_alternative<app::MutationResponse<core::Id>>(result));
  const auto& response = std::get<app::MutationResponse<core::Id>>(result);
  EXPECT_EQ(response.mutation_id, mutation_id);
  ASSERT_TRUE(std::holds_alternative<app::MutationFailure>(response.result));
  const auto& failure = std::get<app::MutationFailure>(response.result);
  EXPECT_EQ(failure.status, app::AuthoritativeChangeStatus::Applied);
  EXPECT_EQ(failure.evidence.entry_id, entry_id);
  ASSERT_EQ(failure.evidence.steps.size(), 1u);
  EXPECT_EQ(failure.evidence.steps[0].observation, app::StepObservation::DurabilityConfirmed);
}

TEST(MutationContract, ConfirmedUnknownDiffersFromMissingResponseForTheSameIntention) {
  const auto mutation_id = app::MutationId::generate();
  const auto entry_id = core::Id::generate();
  app::MutationResult<std::monostate> confirmed = app::MutationResponse<std::monostate>{
      mutation_id,
      app::MutationFailure{
          {app::ErrorCategory::FilesystemFailure, app::ErrorReason::IoFailure,
           "Durability could not be established", {app::ErrorAction::Dismiss}},
          app::AuthoritativeChangeStatus::Unknown,
          {entry_id, std::nullopt, std::nullopt,
           {{app::MutationStep::WriteEntry, app::StepObservation::Published}}}}};
  app::MutationResult<std::monostate> unconfirmed = app::NoMutationResponse{
      mutation_id,
      {app::ErrorCategory::ConnectionFailure, app::ErrorReason::ConnectionLost,
       "Connection lost", {app::ErrorAction::Reconnect}},
      app::SubmissionState::MayHaveBeenSent};

  ASSERT_TRUE(std::holds_alternative<app::MutationResponse<std::monostate>>(confirmed));
  const auto& response = std::get<app::MutationResponse<std::monostate>>(confirmed);
  EXPECT_EQ(std::get<app::MutationFailure>(response.result).status,
            app::AuthoritativeChangeStatus::Unknown);
  ASSERT_TRUE(std::holds_alternative<app::NoMutationResponse>(unconfirmed));
  EXPECT_EQ(std::get<app::NoMutationResponse>(unconfirmed).mutation_id, response.mutation_id);
  EXPECT_EQ(std::get<app::NoMutationResponse>(unconfirmed).submission,
            app::SubmissionState::MayHaveBeenSent);
}

TEST(MutationContract, ConfirmedSuccessRetainsMutationAndCreatedEntryIdentities) {
  const auto mutation_id = app::MutationId::generate();
  const auto entry_id = core::Id::generate();
  app::MutationResult<core::Id> result = app::MutationResponse<core::Id>{mutation_id, entry_id};

  const auto& response = std::get<app::MutationResponse<core::Id>>(result);
  EXPECT_EQ(response.mutation_id, mutation_id);
  EXPECT_EQ(std::get<core::Id>(response.result), entry_id);
}

}  // namespace
