// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.payments;

import static org.chromium.chrome.browser.payments.PaymentRequestTestRule.DELAYED_RESPONSE;
import static org.chromium.chrome.browser.payments.PaymentRequestTestRule.HAVE_INSTRUMENTS;
import static org.chromium.chrome.browser.payments.PaymentRequestTestRule.IMMEDIATE_RESPONSE;
import static org.chromium.chrome.browser.payments.PaymentRequestTestRule.NO_INSTRUMENTS;

import android.support.test.filters.MediumTest;

import org.junit.Before;
import org.junit.ClassRule;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Feature;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.payments.PaymentRequestTestRule.MainActivityStartCallback;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.ui.DisableAnimationsTestRule;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeoutException;

/**
 * A payment integration test for checking whether user can make a payment using a payment app.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE,
        "enable-features=" + ChromeFeatureList.PER_METHOD_CAN_MAKE_PAYMENT_QUOTA})
public class PaymentRequestPaymentAppCanMakePaymentQueryTest implements MainActivityStartCallback {
    // Disable animations to reduce flakiness.
    @ClassRule
    public static DisableAnimationsTestRule sNoAnimationsRule = new DisableAnimationsTestRule();

    @Rule
    public PaymentRequestTestRule mPaymentRequestTestRule = new PaymentRequestTestRule(
            "payment_request_can_make_payment_query_bobpay_test.html", this);

    @Override
    public void onMainActivityStarted() throws InterruptedException, ExecutionException,
            TimeoutException {}

    @Before
    public void setUp() {
        PaymentRequestImpl.setIsLocalCanMakePaymentQueryQuotaEnforcedForTest();
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyNoBobPayInstalled()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});

        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyBobPayInstalledLater()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});

        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, IMMEDIATE_RESPONSE);

        Thread.sleep(10000);
        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        Thread.sleep(10000);
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("enable-features=PaymentRequestHasEnrolledInstrument")
    public void testBobPayInstalledLater()
            throws InterruptedException, ExecutionException, TimeoutException {
        // hasEnrolledInstrument returns false, since BobPay is not installed.
        mPaymentRequestTestRule.openPageAndClickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});

        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, IMMEDIATE_RESPONSE);
        Thread.sleep(10000);

        // hasEnrolledInstrument returns true now for BobPay, but still returns false for AlicePay.
        mPaymentRequestTestRule.clickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyNoInstrumentsInFastBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.installPaymentApp(NO_INSTRUMENTS, IMMEDIATE_RESPONSE);
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});

        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("enable-features=PaymentRequestHasEnrolledInstrument")
    public void testNoInstrumentsInFastBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.installPaymentApp(NO_INSTRUMENTS, IMMEDIATE_RESPONSE);

        // canMakePayment returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.openPageAndClickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});

        // hasEnrolledInstrument returns false for BobPay (installed but no instrument) and
        // false for AlicePay (not installed).
        mPaymentRequestTestRule.clickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyNoInstrumentsInSlowBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.installPaymentApp(NO_INSTRUMENTS, DELAYED_RESPONSE);
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});

        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("enable-features=PaymentRequestHasEnrolledInstrument")
    public void testNoInstrumentsInSlowBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        // Install BobPay.
        mPaymentRequestTestRule.installPaymentApp(NO_INSTRUMENTS, DELAYED_RESPONSE);

        // canMakePayment returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.openPageAndClickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});

        // hasEnrolledInstrument returns false for BobPay (installed but no instrument) and
        // false for AlicePay (not installed).
        mPaymentRequestTestRule.clickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"false, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyPayViaFastBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, IMMEDIATE_RESPONSE);
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, true"});

        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("enable-features=PaymentRequestHasEnrolledInstrument")
    public void testPayViaFastBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        // Install BobPay.
        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, IMMEDIATE_RESPONSE);

        // canMakePayment returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.openPageAndClickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});

        // hasEnrolledInstrument returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.clickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("disable-features=PaymentRequestHasEnrolledInstrument")
    public void testLegacyPayViaSlowBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, DELAYED_RESPONSE);
        mPaymentRequestTestRule.openPageAndClickBuyAndWait(
                mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, true"});

        mPaymentRequestTestRule.clickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }

    @Test
    @MediumTest
    @Feature({"Payments"})
    @CommandLineFlags.Add("enable-features=PaymentRequestHasEnrolledInstrument")
    public void testPayViaSlowBobPay()
            throws InterruptedException, ExecutionException, TimeoutException {
        // Install BobPay.
        mPaymentRequestTestRule.installPaymentApp(HAVE_INSTRUMENTS, DELAYED_RESPONSE);

        // canMakePayment returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.openPageAndClickNodeAndWait(
                "otherBuy", mPaymentRequestTestRule.getCanMakePaymentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});

        // hasEnrolledInstrument returns true for BobPay and false for AlicePay.
        mPaymentRequestTestRule.clickNodeAndWait("hasEnrolledInstrument",
                mPaymentRequestTestRule.getHasEnrolledInstrumentQueryResponded());
        mPaymentRequestTestRule.expectResultContains(new String[] {"true, false"});
    }
}
