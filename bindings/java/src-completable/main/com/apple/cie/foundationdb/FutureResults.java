/*
 * FutureResults.java
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.apple.cie.foundationdb;


class FutureResults extends NativeFuture<RangeResultInfo> {
	FutureResults(long cPtr) {
		super(cPtr);
		registerMarshalCallback();
	}

	@Override
	protected void postMarshal() {
		// We can't dispose because this class actually marshals on-demand
	}

	@Override
	public RangeResultInfo getIfDone_internal() throws FDBException {
		FDBException err = Future_getError(cPtr);

		if(!err.isSuccess()) {
			throw err;
		}

		return new RangeResultInfo(this);
	}

	public RangeResultSummary getSummary() {
		return FutureResults_getSummary(cPtr);
	}

	public RangeResult getResults() {
		return FutureResults_get(cPtr);
	}

	private native RangeResultSummary FutureResults_getSummary(long ptr) throws FDBException;
	private native RangeResult FutureResults_get(long cPtr) throws FDBException;
}
