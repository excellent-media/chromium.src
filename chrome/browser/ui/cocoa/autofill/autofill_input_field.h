// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_COCOA_AUTOFILL_AUTOFILL_INPUT_FIELD_H_
#define CHROME_BROWSER_UI_COCOA_AUTOFILL_AUTOFILL_INPUT_FIELD_H_

// Access to cell state for autofill input controls.
@protocol AutofillInputCell<NSObject>

@property(nonatomic, assign) BOOL invalid;
@property(nonatomic, copy) NSString* fieldValue;

@end

// Delegate to handle editing events on the AutofillInputFields.
@protocol AutofillInputDelegate<NSObject>

// The user made changes to the value in the field. This is only invoked by
// AutofillTextFields.
- (void)didChange:(id)sender;

// The user is done with this field. This indicates a loss of firstResponder
// status.
- (void)didEndEditing:(id)sender;

@end

// Protocol to allow access to any given input field in an Autofill dialog, no
// matter what the underlying control is. All controls act as proxies for their
// cells, so inherits from AutofillInputCell.
@protocol AutofillInputField<AutofillInputCell>

@property(nonatomic, assign) id<AutofillInputDelegate> delegate;

@end

#endif  // CHROME_BROWSER_UI_COCOA_AUTOFILL_AUTOFILL_INPUT_FIELD_H_
