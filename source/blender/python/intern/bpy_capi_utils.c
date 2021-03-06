/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file contains Blender/Python utility functions to help implementing API's.
 * This is not related to a particular module.
 */

#include <Python.h>

#include "BLI_dynstr.h"
#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "bpy_capi_utils.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.h"
#include "BKE_report.h"

#include "BLT_translation.h"

#include "../generic/py_capi_utils.h"

short BPy_reports_to_error(ReportList *reports, PyObject *exception, const bool clear)
{
  char *report_str;

  report_str = BKE_reports_string(reports, RPT_ERROR);

  if (clear == true) {
    BKE_reports_clear(reports);
  }

  if (report_str) {
    PyErr_SetString(exception, report_str);
    MEM_freeN(report_str);
  }

  return (report_str == NULL) ? 0 : -1;
}

void BPy_reports_write_stdout(const ReportList *reports, const char *header)
{
  if (header) {
    PySys_WriteStdout("%s\n", header);
  }

  LISTBASE_FOREACH (const Report *, report, &reports->list) {
    PySys_WriteStdout("%s: %s\n", report->typestr, report->message);
  }
}

bool BPy_errors_to_report_ex(ReportList *reports,
                             const char *error_prefix,
                             const bool use_full,
                             const bool use_location)
{
  PyObject *pystring;

  if (!PyErr_Occurred()) {
    return 1;
  }

  /* less hassle if we allow NULL */
  if (reports == NULL) {
    PyErr_Print();
    PyErr_Clear();
    return 1;
  }

  if (use_full) {
    pystring = PyC_ExceptionBuffer();
  }
  else {
    pystring = PyC_ExceptionBuffer_Simple();
  }

  if (pystring == NULL) {
    BKE_report(reports, RPT_ERROR, "Unknown py-exception, could not convert");
    return 0;
  }

  if (error_prefix == NULL) {
    /* Not very helpful, better than nothing. */
    error_prefix = "Python";
  }

  if (use_location) {
    const char *filename;
    int lineno;

    PyC_FileAndNum(&filename, &lineno);
    if (filename == NULL) {
      filename = "<unknown location>";
    }

    BKE_reportf(reports,
                RPT_ERROR,
                TIP_("%s: %s\nlocation: %s:%d\n"),
                error_prefix,
                PyUnicode_AsUTF8(pystring),
                filename,
                lineno);

    /* Not exactly needed. Useful for developers tracking down issues. */
    fprintf(stderr,
            TIP_("%s: %s\nlocation: %s:%d\n"),
            error_prefix,
            PyUnicode_AsUTF8(pystring),
            filename,
            lineno);
  }
  else {
    BKE_reportf(reports, RPT_ERROR, "%s: %s", error_prefix, PyUnicode_AsUTF8(pystring));
  }

  Py_DECREF(pystring);
  return 1;
}

bool BPy_errors_to_report(ReportList *reports)
{
  return BPy_errors_to_report_ex(reports, NULL, true, true);
}
