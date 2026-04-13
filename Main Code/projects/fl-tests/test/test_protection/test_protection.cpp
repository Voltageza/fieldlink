// Native unit tests for FieldLinkCore's fl_protection module.
//
// These tests run on the host via PlatformIO Unity:
//   pio test -e native
//
// They cover all protection math (imbalance, phase loss, overcurrent,
// dry run) plus the debounce state machine. Every fault path that can
// fire on a real device should be exercisable here without hardware.

#include <unity.h>
#include "fl_protection.h"

void setUp(void) {}
void tearDown(void) {}


// ---------- average3 ----------

void test_average3_balanced(void) {
    TEST_ASSERT_EQUAL_FLOAT(10.0f, fl_prot_average3(10.0f, 10.0f, 10.0f));
}

void test_average3_unbalanced(void) {
    // (9 + 10 + 11) / 3 = 10
    TEST_ASSERT_EQUAL_FLOAT(10.0f, fl_prot_average3(9.0f, 10.0f, 11.0f));
}

void test_average3_all_zero(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fl_prot_average3(0.0f, 0.0f, 0.0f));
}


// ---------- phaseImbalancePct ----------

void test_imbalance_balanced_load(void) {
    // 10A on all three phases → 0% imbalance.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fl_prot_phaseImbalancePct(10.0f, 10.0f, 10.0f));
}

void test_imbalance_moderate(void) {
    // avg = 15, max dev = |20-15| = 5, pct = 5/15*100 = 33.33%
    float pct = fl_prot_phaseImbalancePct(20.0f, 15.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 33.333f, pct);
}

void test_imbalance_one_phase_high(void) {
    // avg = (11+10+9)/3 = 10, max dev = 1, pct = 10%
    float pct = fl_prot_phaseImbalancePct(11.0f, 10.0f, 9.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, pct);
}

void test_imbalance_below_min_returns_zero(void) {
    // Motor stopped: all phases at 0.1A. avg = 0.1 < 0.5 → 0%.
    // (Imbalance on a stopped motor is meaningless — divide-by-near-zero.)
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fl_prot_phaseImbalancePct(0.1f, 0.1f, 0.1f));
}

void test_imbalance_exactly_at_min_threshold(void) {
    // avg = 0.5 is the boundary. Right AT the threshold should return 0.0
    // (the condition is `< min`, not `<= min`).
    float pct = fl_prot_phaseImbalancePct(0.5f, 0.5f, 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pct);
}


// ---------- phaseLoss ----------

void test_phase_loss_motor_stopped_is_not_loss(void) {
    // All phases at 0A = motor off. Must NOT report phase loss.
    TEST_ASSERT_FALSE(fl_prot_phaseLoss(0.0f, 0.0f, 0.0f, 1.0f, 2.0f));
}

void test_phase_loss_single_phase_dropped(void) {
    // L1 and L2 pulling load, L3 dead → phase loss.
    TEST_ASSERT_TRUE(fl_prot_phaseLoss(10.0f, 10.0f, 0.2f, 1.0f, 2.0f));
}

void test_phase_loss_double_phase_dropped(void) {
    // Only L1 left → phase loss (two phases below threshold while one is loaded).
    TEST_ASSERT_TRUE(fl_prot_phaseLoss(10.0f, 0.1f, 0.1f, 1.0f, 2.0f));
}

void test_phase_loss_balanced_running_is_not_loss(void) {
    // 10A on all phases → not phase loss.
    TEST_ASSERT_FALSE(fl_prot_phaseLoss(10.0f, 10.0f, 10.0f, 1.0f, 2.0f));
}

void test_phase_loss_all_phases_below_loss_threshold_but_not_running(void) {
    // 0.5A on all three — below running threshold (2A) → "motor off",
    // not phase loss.
    TEST_ASSERT_FALSE(fl_prot_phaseLoss(0.5f, 0.5f, 0.5f, 1.0f, 2.0f));
}


// ---------- overcurrent ----------

void test_overcurrent_below_threshold(void) {
    TEST_ASSERT_FALSE(fl_prot_overcurrent(9.9f, 10.0f));
}

void test_overcurrent_at_threshold_is_false(void) {
    // Strict greater-than: AT the threshold is not a trip.
    TEST_ASSERT_FALSE(fl_prot_overcurrent(10.0f, 10.0f));
}

void test_overcurrent_above_threshold(void) {
    TEST_ASSERT_TRUE(fl_prot_overcurrent(10.1f, 10.0f));
}


// ---------- dryRun ----------

void test_dryrun_normal_load(void) {
    TEST_ASSERT_FALSE(fl_prot_dryRun(10.0f, 2.0f));
}

void test_dryrun_low_current(void) {
    TEST_ASSERT_TRUE(fl_prot_dryRun(1.0f, 2.0f));
}

void test_dryrun_at_threshold_is_false(void) {
    // Strict less-than: AT the threshold is not a trip.
    TEST_ASSERT_FALSE(fl_prot_dryRun(2.0f, 2.0f));
}


// ---------- debounce state machine ----------

void test_debounce_init(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL_UINT32(0, s.startTimeMs);
}

void test_debounce_condition_false_never_trips(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);
    for (uint32_t t = 0; t < 10000; t += 100) {
        TEST_ASSERT_FALSE(fl_prot_debounceTick(s, false, t, 1000));
    }
}

void test_debounce_zero_delay_trips_immediately(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);
    // delayMs=0 → first true reading trips.
    TEST_ASSERT_TRUE(fl_prot_debounceTick(s, true, 1234, 0));
}

void test_debounce_requires_full_delay(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);

    // Condition becomes true at t=1000, delay is 3000ms.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 1000, 3000));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 2000, 3000));  // +1000
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 3999, 3000));  // +2999
    TEST_ASSERT_TRUE(fl_prot_debounceTick(s, true, 4000, 3000));   // +3000 exactly
}

void test_debounce_exact_delay_boundary(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 500, 1000));
    // 500 + 1000 = 1500. At 1499 still pending; at 1500 should trip.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 1499, 1000));
    TEST_ASSERT_TRUE(fl_prot_debounceTick(s, true, 1500, 1000));
}

void test_debounce_resets_when_condition_clears(void) {
    fl_DebounceState s;
    fl_prot_debounceInit(s);
    // Condition true from 1000 to 2000, then clears.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 1000, 3000));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 2000, 3000));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, false, 2500, 3000));
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL_UINT32(0, s.startTimeMs);

    // Condition reappears at 3000 — timer must restart from zero.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 3000, 3000));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(s, true, 5000, 3000));  // +2000 since restart
    TEST_ASSERT_TRUE(fl_prot_debounceTick(s, true, 6000, 3000));   // +3000 since restart
}


// ---------- integrated Adam-style protection scenarios ----------
//
// These tests simulate the full Adam-style protection flow — SDM630 reads
// three phase currents, we feed them into fl_protection, and check that
// the fault triggers exactly when expected.

void test_adam_balanced_load_no_faults(void) {
    const float IL1 = 12.0f, IL2 = 12.0f, IL3 = 12.0f;
    const float MAX_A = 20.0f;
    const float IMB_PCT_TRIP = 10.0f;
    const float PHASE_LOSS_A = 2.0f;
    const float RUNNING_A = 4.0f;

    float imb = fl_prot_phaseImbalancePct(IL1, IL2, IL3);
    TEST_ASSERT_FALSE(imb > IMB_PCT_TRIP);
    TEST_ASSERT_FALSE(fl_prot_phaseLoss(IL1, IL2, IL3, PHASE_LOSS_A, RUNNING_A));
    TEST_ASSERT_FALSE(fl_prot_overcurrent(IL1, MAX_A));
    TEST_ASSERT_FALSE(fl_prot_overcurrent(IL2, MAX_A));
    TEST_ASSERT_FALSE(fl_prot_overcurrent(IL3, MAX_A));
}

void test_adam_overcurrent_on_l2_only(void) {
    const float IL1 = 12.0f, IL2 = 25.0f, IL3 = 12.0f;
    const float MAX_A = 20.0f;
    // L2 over limit → per-phase overcurrent trips.
    TEST_ASSERT_FALSE(fl_prot_overcurrent(IL1, MAX_A));
    TEST_ASSERT_TRUE(fl_prot_overcurrent(IL2, MAX_A));
    TEST_ASSERT_FALSE(fl_prot_overcurrent(IL3, MAX_A));
}

void test_adam_phase_loss_scenario(void) {
    // L3 fuse blown under load. L1/L2 at 15A, L3 at 0.1A.
    const float IL1 = 15.0f, IL2 = 15.0f, IL3 = 0.1f;
    const float PHASE_LOSS_A = 2.0f;
    const float RUNNING_A = 4.0f;
    TEST_ASSERT_TRUE(fl_prot_phaseLoss(IL1, IL2, IL3, PHASE_LOSS_A, RUNNING_A));
}

void test_adam_imbalance_trip(void) {
    // avg = (20+20+10)/3 = 16.67, max dev = |10-16.67| = 6.67, pct = 40%
    const float IL1 = 20.0f, IL2 = 20.0f, IL3 = 10.0f;
    const float IMB_PCT_TRIP = 10.0f;
    float imb = fl_prot_phaseImbalancePct(IL1, IL2, IL3);
    TEST_ASSERT_TRUE(imb > IMB_PCT_TRIP);
}

void test_adam_dry_run_uses_avg_current(void) {
    // Dry run: avg current drops below dry threshold after successful start.
    const float IL1 = 0.5f, IL2 = 0.5f, IL3 = 0.5f;
    const float DRY_A = 2.0f;
    float avg = fl_prot_average3(IL1, IL2, IL3);
    TEST_ASSERT_TRUE(fl_prot_dryRun(avg, DRY_A));
}


// ---------- integrated Eve-style protection (regression for BUG-001 fix) ----------
//
// Eve controls three independent motors; each motor has its own current
// channel and its own debounce state. These tests mirror what Eve's
// main.cpp does today, so the fl_protection refactor must not regress
// Eve's fault behavior.

void test_eve_pump_overcurrent_with_debounce(void) {
    fl_DebounceState pump1_oc;
    fl_prot_debounceInit(pump1_oc);
    const float MAX_A = 15.0f;
    const uint32_t DELAY_MS = 3000;

    // t=0: pump draws 20A (over limit). Debounce starts, does NOT trip yet.
    bool oc = fl_prot_overcurrent(20.0f, MAX_A);
    TEST_ASSERT_TRUE(oc);
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, oc, 0, DELAY_MS));

    // t=2999: still pending.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, oc, 2999, DELAY_MS));

    // t=3000: trip.
    TEST_ASSERT_TRUE(fl_prot_debounceTick(pump1_oc, oc, 3000, DELAY_MS));
}

void test_eve_pump_overcurrent_transient_does_not_trip(void) {
    fl_DebounceState pump1_oc;
    fl_prot_debounceInit(pump1_oc);
    const float MAX_A = 15.0f;
    const uint32_t DELAY_MS = 3000;

    // 500ms spike above the limit, then back to normal → must NOT trip.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, true, 0, DELAY_MS));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, true, 500, DELAY_MS));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, false, 501, DELAY_MS));
    // Now run well past the original delay — still must not trip because
    // the timer was reset.
    TEST_ASSERT_FALSE(fl_prot_debounceTick(pump1_oc, false, 5000, DELAY_MS));
}

void test_eve_pump_dryrun_independent_state(void) {
    // Three pumps, three debounce states. Pump 2 dry-running, pumps 1 and 3 fine.
    fl_DebounceState p1, p2, p3;
    fl_prot_debounceInit(p1);
    fl_prot_debounceInit(p2);
    fl_prot_debounceInit(p3);

    const float DRY_A = 2.0f;
    const uint32_t DELAY_MS = 5000;

    bool dry1 = fl_prot_dryRun(10.0f, DRY_A);  // pumping fine
    bool dry2 = fl_prot_dryRun(0.5f, DRY_A);   // dry
    bool dry3 = fl_prot_dryRun(11.0f, DRY_A);  // pumping fine

    // t=0
    TEST_ASSERT_FALSE(fl_prot_debounceTick(p1, dry1, 0, DELAY_MS));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(p2, dry2, 0, DELAY_MS));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(p3, dry3, 0, DELAY_MS));

    // t=5000
    TEST_ASSERT_FALSE(fl_prot_debounceTick(p1, dry1, 5000, DELAY_MS));
    TEST_ASSERT_TRUE(fl_prot_debounceTick(p2, dry2, 5000, DELAY_MS));
    TEST_ASSERT_FALSE(fl_prot_debounceTick(p3, dry3, 5000, DELAY_MS));
}


// ---------- test runner ----------

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();

    // average3
    RUN_TEST(test_average3_balanced);
    RUN_TEST(test_average3_unbalanced);
    RUN_TEST(test_average3_all_zero);

    // phaseImbalancePct
    RUN_TEST(test_imbalance_balanced_load);
    RUN_TEST(test_imbalance_moderate);
    RUN_TEST(test_imbalance_one_phase_high);
    RUN_TEST(test_imbalance_below_min_returns_zero);
    RUN_TEST(test_imbalance_exactly_at_min_threshold);

    // phaseLoss
    RUN_TEST(test_phase_loss_motor_stopped_is_not_loss);
    RUN_TEST(test_phase_loss_single_phase_dropped);
    RUN_TEST(test_phase_loss_double_phase_dropped);
    RUN_TEST(test_phase_loss_balanced_running_is_not_loss);
    RUN_TEST(test_phase_loss_all_phases_below_loss_threshold_but_not_running);

    // overcurrent
    RUN_TEST(test_overcurrent_below_threshold);
    RUN_TEST(test_overcurrent_at_threshold_is_false);
    RUN_TEST(test_overcurrent_above_threshold);

    // dryRun
    RUN_TEST(test_dryrun_normal_load);
    RUN_TEST(test_dryrun_low_current);
    RUN_TEST(test_dryrun_at_threshold_is_false);

    // debounce
    RUN_TEST(test_debounce_init);
    RUN_TEST(test_debounce_condition_false_never_trips);
    RUN_TEST(test_debounce_zero_delay_trips_immediately);
    RUN_TEST(test_debounce_requires_full_delay);
    RUN_TEST(test_debounce_exact_delay_boundary);
    RUN_TEST(test_debounce_resets_when_condition_clears);

    // Adam scenarios
    RUN_TEST(test_adam_balanced_load_no_faults);
    RUN_TEST(test_adam_overcurrent_on_l2_only);
    RUN_TEST(test_adam_phase_loss_scenario);
    RUN_TEST(test_adam_imbalance_trip);
    RUN_TEST(test_adam_dry_run_uses_avg_current);

    // Eve scenarios
    RUN_TEST(test_eve_pump_overcurrent_with_debounce);
    RUN_TEST(test_eve_pump_overcurrent_transient_does_not_trip);
    RUN_TEST(test_eve_pump_dryrun_independent_state);

    return UNITY_END();
}
