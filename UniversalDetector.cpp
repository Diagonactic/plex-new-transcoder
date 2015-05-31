#import "UniversalDetector.h"

#import "nscore.h"
#import "nsUniversalDetector.h"
#import "nsCharSetProber.h"

//
// This method is taken from:
//
// http://trac.cod3r.com/perian/browser/trunk/UniversalDetector/UniversalDetector.mm?rev=1085
//
// It's covered by the LGPL v2.1
//
// http://www.gnu.org/licenses/lgpl-2.1.html
//
// from that project. Copyright © 2006-2009 Perian Project. All rights reserved.
//
class wrappedUniversalDetector:public nsUniversalDetector
{
	public:
	void Report(const char* aCharset) {};
	
	const char *charset(float &confidence)
	{
		if(!mGotData)
		{
			confidence=0;
			return 0;
		}

		if(mDetectedCharset)
		{
			confidence=1;
			return mDetectedCharset;
		}

		switch(mInputState)
		{
			case eHighbyte:
			{
				float proberConfidence;
				float maxProberConfidence = (float)0.0;
				PRInt32 maxProber = 0;

				for (PRInt32 i = 0; i < NUM_OF_CHARSET_PROBERS; i++)
				{
					proberConfidence = mCharSetProbers[i]->GetConfidence();
					if (proberConfidence > maxProberConfidence)
					{
						maxProberConfidence = proberConfidence;
						maxProber = i;
					}
				}

				confidence=maxProberConfidence;
				return mCharSetProbers[maxProber]->GetCharSetName();
			}
			break;

			case ePureAscii:
				confidence=0;
				return "US-ASCII";
		}

		confidence=0;
		return 0;
	}
};

const char *mimeCharsetForData(char *data, size_t len, float *confidenceOut)
{
	wrappedUniversalDetector detector = wrappedUniversalDetector();
	detector.HandleData(data,len);
	float confidence;
	const char *cstr=detector.charset(confidence);
	if (confidenceOut)
	{
		*confidenceOut = confidence;
	}
	return cstr;
}
