/*
 * plv8.cc : Postgres from/to v8 data converters.
 */
#include "plv8.h"

extern "C" {
#define delete		delete_
#define namespace	namespace_
#define	typeid		typeid_
#define	typename	typename_
#define	using		using_

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#undef delete
#undef namespace
#undef typeid
#undef typename
#undef using
} // extern "C"

//#define CHECK_INTEGER_OVERFLOW

using namespace v8;

static Datum ToScalarDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Datum ToArrayDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Datum ToRecordDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type);
static Handle<v8::Value> ToScalarValue(Datum datum, bool isnull, plv8_type *type);
static Handle<v8::Value> ToArrayValue(Datum datum, bool isnull, plv8_type *type);
static Handle<v8::Value> ToRecordValue(Datum datum, bool isnull, plv8_type *type);
static double TimestampTzToEpoch(TimestampTz tm);
static Datum EpochToTimestampTz(double epoch);
static double DateToEpoch(DateADT date);
static Datum EpochToDate(double epoch);

Datum
ToDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	if (type->category == TYPCATEGORY_ARRAY)
		return ToArrayDatum(value, isnull, type);
	else
		return ToScalarDatum(value, isnull, type);
}

static Datum
ToScalarDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	if (type->category == TYPCATEGORY_COMPOSITE)
		return ToRecordDatum(value, isnull, type);

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	*isnull = false;
	switch (type->typid)
	{
	case OIDOID:
		if (value->IsNumber())
			return ObjectIdGetDatum(value->Uint32Value());
		break;
	case BOOLOID:
		if (value->IsBoolean())
			return BoolGetDatum(value->BooleanValue());
		break;
	case INT2OID:
		if (value->IsNumber())
#ifdef CHECK_INTEGER_OVERFLOW
			return DirectFunctionCall1(int82,
					Int64GetDatum(value->IntegerValue()));
#else
			return Int16GetDatum((int16) value->Int32Value());
#endif
		break;
	case INT4OID:
		if (value->IsNumber())
#ifdef CHECK_INTEGER_OVERFLOW
			return DirectFunctionCall1(int84,
					Int64GetDatum(value->IntegerValue()));
#else
			return Int32GetDatum((int32) value->Int32Value());
#endif
		break;
	case INT8OID:
		if (value->IsNumber())
			return Int64GetDatum((int64) value->IntegerValue());
		break;
	case FLOAT4OID:
		if (value->IsNumber())
			return Float4GetDatum((float4) value->NumberValue());
		break;
	case FLOAT8OID:
		if (value->IsNumber())
			return Float8GetDatum((float8) value->NumberValue());
		break;
	case NUMERICOID:
		if (value->IsNumber())
			return DirectFunctionCall1(float8_numeric,
					Float8GetDatum((float8) value->NumberValue()));
		break;
	case DATEOID:
		if (value->IsDate())
			return EpochToDate(value->NumberValue());
		break;
	case TIMESTAMPOID:
	case TIMESTAMPTZOID:
		if (value->IsDate())
			return EpochToTimestampTz(value->NumberValue());
		break;
	}

	/* Use lexical cast for non-numeric types. */
	int			encoding = GetDatabaseEncoding();
	CString		str(value);
	Datum		result;

	PG_TRY();
	{
		if (type->fn_input.fn_addr == NULL)
		{
			Oid    input_func;

			getTypeInputInfo(type->typid, &input_func, &type->ioparam);
			fmgr_info(input_func, &type->fn_input);
		}
		result = InputFunctionCall(&type->fn_input, str, type->ioparam, -1);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return result;
}

static Datum
ToArrayDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	int			length;
	Datum	   *values;
	bool	   *nulls;
	int			ndims[1];
	int			lbs[] = {1};
	ArrayType  *result;

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	Handle<Array> array(Handle<Array>::Cast(value));
	if (array.IsEmpty())
		throw js_error("value is not an Array");

	length = array->Length();
	values = (Datum *) palloc(sizeof(Datum) * length);
	nulls = (bool *) palloc(sizeof(bool) * length);
	ndims[0] = length;
	for (int i = 0; i < length; i++)
		values[i] = ToScalarDatum(array->Get(i), &nulls[i], type);

	result = construct_md_array(values, nulls, 1, ndims, lbs,
				type->typid, type->len, type->byval, type->align);
	pfree(values);
	pfree(nulls);

	*isnull = false;
	return PointerGetDatum(result);
}

static Datum
ToRecordDatum(Handle<v8::Value> value, bool *isnull, plv8_type *type)
{
	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	throw js_error("JSON to record is not implemented yet");
}

Handle<v8::Value>
ToValue(Datum datum, bool isnull, plv8_type *type)
{
	if (type->category == TYPCATEGORY_ARRAY)
		return ToArrayValue(datum, isnull, type);
	else
		return ToScalarValue(datum, isnull, type);
}

static Handle<v8::Value>
ToScalarValue(Datum datum, bool isnull, plv8_type *type)
{
	if (isnull)
		return Null();

	if (type->category == TYPCATEGORY_COMPOSITE)
		return ToRecordValue(datum, isnull, type);

	switch (type->typid)
	{
	case OIDOID:
		return Uint32::New(DatumGetObjectId(datum));
	case BOOLOID:
		return Boolean::New((bool) DatumGetBool(datum));
	case INT2OID:
		return Int32::New(DatumGetInt16(datum));
	case INT4OID:
		return Int32::New(DatumGetInt32(datum));
	case INT8OID:
		return Number::New(DatumGetInt64(datum));
	case FLOAT4OID:
		return Number::New(DatumGetFloat4(datum));
	case FLOAT8OID:
		return Number::New(DatumGetFloat8(datum));
	case NUMERICOID:
		return Number::New(DatumGetFloat8(
			DirectFunctionCall1(numeric_float8, datum)));
	case DATEOID:
		return Date::New(DateToEpoch(DatumGetDateADT(datum)));
	case TIMESTAMPOID:
	case TIMESTAMPTZOID:
		return Date::New(TimestampTzToEpoch(DatumGetTimestampTz(datum)));
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID:
	case XMLOID:
	{
		void	   *p = PG_DETOAST_DATUM_PACKED(datum);
		const char *str = VARDATA_ANY(p);
		int			len = VARSIZE_ANY_EXHDR(p);

		Handle<String>	result = ToString(str, len);

		if (p != DatumGetPointer(datum))
			pfree(p);	// free if detoasted
		return result;
	}
	default:
		return ToString(datum, type);
	}
}

static Handle<v8::Value>
ToArrayValue(Datum datum, bool isnull, plv8_type *type)
{
	Datum	   *values;
	bool	   *nulls;
	int			nelems;

	if (isnull)
		return Null();

	deconstruct_array(DatumGetArrayTypeP(datum),
						type->typid, type->len, type->byval, type->align,
						&values, &nulls, &nelems);
	Handle<Array>  result = Array::New(nelems);
	for (int i = 0; i < nelems; i++)
		result->Set(i, ToScalarValue(values[i], nulls[i], type));

	return result;
}

static Handle<v8::Value>
ToRecordValue(Datum datum, bool isnull, plv8_type *type)
{
	if (isnull)
		return Null();

	HeapTupleHeader	rec = DatumGetHeapTupleHeader(datum);
	Oid				tupType;
	int32			tupTypmod;
	TupleDesc		tupdesc;
	HeapTupleData	tuple;

	PG_TRY();
	{
		/* Extract type info from the tuple itself */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
		tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Converter	conv(tupdesc);

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	Handle<v8::Value> result = conv.ToValue(&tuple);

	ReleaseTupleDesc(tupdesc);

	return result;
}

Handle<String>
ToString(Datum value, plv8_type *type)
{
	int		encoding = GetDatabaseEncoding();
	char   *str;

	PG_TRY();
	{
		if (type->fn_output.fn_addr == NULL)
		{
			Oid		output_func;
			bool	isvarlen;

			getTypeOutputInfo(type->typid, &output_func, &isvarlen);
			fmgr_info(output_func, &type->fn_output);
		}
		str = OutputFunctionCall(&type->fn_output, value);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Handle<String>	result =
		encoding == PG_UTF8
			? String::New(str)
			: ToString(str, strlen(str), encoding);
	pfree(str);

	return result;
}

Handle<String>
ToString(const char *str, int len, int encoding)
{
	char		   *utf8;

	if (len < 0)
		len = strlen(str);

	PG_TRY();
	{
		utf8 = (char *) pg_do_encoding_conversion(
					(unsigned char *) str, len, encoding, PG_UTF8);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (utf8 != str)
		len = strlen(utf8);
	Handle<String> result = String::New(utf8, len);
	if (utf8 != str)
		pfree(utf8);
	return result;
}

/*
 * Convert utf8 text to database encoded text.
 * The result could be same as utf8 input, or palloc'ed one.
 */
char *
ToCString(const String::Utf8Value &value)
{
	char *str = const_cast<char *>(*value);
	if (str == NULL)
		return NULL;

	int    encoding = GetDatabaseEncoding();
	if (encoding == PG_UTF8)
		return str;

	PG_TRY();
	{
		str = (char *) pg_do_encoding_conversion(
				(unsigned char *) str, strlen(str), PG_UTF8, encoding);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return str;
}

/*
 * Convert utf8 text to database encoded text.
 * The result is always palloc'ed one.
 */
char *
ToCStringCopy(const String::Utf8Value &value)
{
	char *str;
	const char *utf8 = *value;
	if (utf8 == NULL)
		return NULL;

	PG_TRY();
	{
		int	encoding = GetDatabaseEncoding();
		str = (char *) pg_do_encoding_conversion(
				(unsigned char *) utf8, strlen(utf8), PG_UTF8, encoding);
		if (str == utf8)
			str = pstrdup(utf8);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return str;
}

/*
 * Since v8 represents a Date object using a double value in msec from unix epoch,
 * we need to shift the epoch and adjust the time unit.
 */
static double
TimestampTzToEpoch(TimestampTz tm)
{
	double		epoch;

	// TODO: check if TIMESTAMP_NOBEGIN or NOEND
#ifdef HAVE_INT64_TIMESTAMP
	epoch = (double) tm / 1000.0;
#else
	epoch = (double) tm * 1000.0;
#endif

	return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

static Datum
EpochToTimestampTz(double epoch)
{
	epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
	return Int64GetDatum((int64) epoch * 1000);
#else
	return Float8GetDatum(epoch / 1000.0);
#endif
}

static double
DateToEpoch(DateADT date)
{
	double		epoch;

	// TODO: check if DATE_NOBEGIN or NOEND
#ifdef HAVE_INT64_TIMESTAMP
	epoch = (double) date * USECS_PER_DAY / 1000.0;
#else
	epoch = (double) date * SECS_PER_DAY * 1000.0;
#endif

	return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

static Datum
EpochToDate(double epoch)
{
	epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
	return Int64GetDatum((int64) (epoch * 1000 / USECS_PER_DAY));
#else
	return Float8GetDatum(epoch / 1000.0 / SECS_PER_DAY);
#endif
}

CString::CString(Handle<v8::Value> value) : m_utf8(value)
{
	m_str = ToCString(m_utf8);
}

CString::~CString()
{
	if (m_str != *m_utf8)
		pfree(m_str);
}
