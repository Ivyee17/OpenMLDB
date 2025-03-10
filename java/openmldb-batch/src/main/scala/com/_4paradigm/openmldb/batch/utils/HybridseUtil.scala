/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com._4paradigm.openmldb.batch.utils

import java.util

import com._4paradigm.hybridse.`type`.TypeOuterClass.{ColumnDef, Database, TableDef}
import com._4paradigm.hybridse.node.ConstNode
import com._4paradigm.hybridse.sdk.UnsupportedHybridSeException
import com._4paradigm.hybridse.vm.{PhysicalLoadDataNode, PhysicalOpNode, PhysicalSelectIntoNode}
import com._4paradigm.openmldb.proto
import com._4paradigm.openmldb.proto.Common
import org.apache.spark.sql.functions.{col, first}
import org.apache.spark.sql.types.{DataType, LongType, StructField, StructType, TimestampType}
import org.apache.spark.sql.{DataFrame, DataFrameReader, Row, SparkSession}
import org.slf4j.LoggerFactory

import scala.collection.JavaConverters.asScalaBufferConverter
import scala.collection.convert.ImplicitConversions.`collection AsScalaIterable`
import scala.collection.mutable


object HybridseUtil {
  private val logger = LoggerFactory.getLogger(this.getClass)

  def getOutputSchemaSlices(node: PhysicalOpNode, enableUnsafeRowOpt: Boolean): Array[StructType] = {
    if (enableUnsafeRowOpt) {
      // If enabling UnsafeRowOpt, return row with one slice
      val columnDefs = node.GetOutputSchema()
      Array(getSparkSchema(columnDefs))
    } else {
      (0 until node.GetOutputSchemaSourceSize().toInt).map(i => {
        val columnDefs = node.GetOutputSchemaSource(i).GetSchema()
        getSparkSchema(columnDefs)
      }).toArray
    }
  }

  def getDatabases(tableMap: mutable.Map[String, mutable.Map[String, DataFrame]]): List[Database] = {
    tableMap.map { case (dbName, tableDfMap) =>
      getDatabase(dbName, tableDfMap.toMap)
    }.toList
  }

  def getDatabase(databaseName: String, dict: Map[String, DataFrame]): Database = {
    val databaseBuilder = Database.newBuilder()
    databaseBuilder.setName(databaseName)
    dict.foreach { case (name, df) =>
      databaseBuilder.addTables(getTableDef(name, df))
    }
    databaseBuilder.build()
  }

  def getTableDef(tableName: String, dataFrame: DataFrame): TableDef = {
    val tblBulder = TableDef.newBuilder()
    dataFrame.schema.foreach(field => {
      tblBulder.addColumns(ColumnDef.newBuilder()
        .setName(field.name)
        .setIsNotNull(!field.nullable)
        .setType(DataTypeUtil.sparkTypeToHybridseProtoType(field.dataType))
        .build()
      )
    })
    tblBulder.setName(tableName)
    tblBulder.build()
  }

  def getHybridseSchema(structType: StructType): java.util.List[ColumnDef] = {
    val list = new util.ArrayList[ColumnDef]()
    structType.foreach(field => {
      list.add(ColumnDef.newBuilder()
        .setName(field.name)
        .setIsNotNull(!field.nullable)
        .setType(DataTypeUtil.sparkTypeToHybridseProtoType(field.dataType)).build())
    })
    list
  }

  def getSparkSchema(columns: java.util.List[ColumnDef]): StructType = {
    StructType(columns.asScala.map(col => {
      StructField(col.getName, DataTypeUtil.hybridseProtoTypeToSparkType(col.getType), !col.getIsNotNull)
    }))
  }

  def createGroupKeyComparator(keyIdxs: Array[Int]): (Row, Row) => Boolean = {
    if (keyIdxs.length == 1) {
      val idx = keyIdxs(0)
      (row1, row2) => {
        row1.get(idx) != row2.get(idx)
      }
    } else {
      (row1, row2) => {
        keyIdxs.exists(i => row1.get(i) != row2.get(i))
      }
    }
  }

  def parseOption(node: ConstNode, default: String, f: (ConstNode, String) => String): String = {
    f(node, default)
  }

  def getBoolOrDefault(node: ConstNode, default: String): String = {
    if (node != null) {
      node.GetBool().toString
    } else {
      default
    }
  }

  def updateOptionsMap(options: mutable.Map[String, String], node: ConstNode, name: String, getValue: ConstNode =>
    String): Unit = {
    if (node != null) {
      options += (name -> getValue(node))
    }
  }

  def getStringOrDefault(node: ConstNode, default: String): String = {
    if (node != null) {
      node.GetStr()
    } else {
      default
    }
  }

  def getBool(node: ConstNode): String = {
    node.GetBool().toString
  }

  def getStr(node: ConstNode): String = {
    node.GetStr()
  }

  def getOptionFromNode[T](node: T, name: String): ConstNode = {
    node match {
      case node1: PhysicalSelectIntoNode => node1.GetOption(name)
      case node1: PhysicalLoadDataNode => node1.GetOption(name)
      case _ => throw new UnsupportedHybridSeException(s"${node.getClass} doesn't support GetOption method")
    }
  }

  def parseOptions[T](node: T): (String, Map[String, String], String, Option[Boolean]) = {
    // load data: read format, select into: write format
    val format = parseOption(getOptionFromNode(node, "format"), "csv", getStringOrDefault).toLowerCase
    require(format.equals("csv") || format.equals("parquet"))

    // load data: read options, select into: write options
    val options: mutable.Map[String, String] = mutable.Map()
    // default values:
    // delimiter -> sep: ,
    // header: true(different with spark)
    // null_value -> nullValue: null(different with spark)
    // quote: '\0'(means no quote, the same with spark quote "empty string")
    options += ("header" -> "true")
    options += ("nullValue" -> "null")
    updateOptionsMap(options, getOptionFromNode(node, "delimiter"), "sep", getStr)
    updateOptionsMap(options, getOptionFromNode(node, "header"), "header", getBool)
    updateOptionsMap(options, getOptionFromNode(node, "null_value"), "nullValue", getStr)
    updateOptionsMap(options, getOptionFromNode(node, "quote"), "quote", getStr)

    // load data: write mode(load data may write to offline storage or online storage, needs mode too)
    // select into: write mode
    val modeStr = parseOption(getOptionFromNode(node, "mode"), "error_if_exists", HybridseUtil
      .getStringOrDefault).toLowerCase
    val mode = modeStr match {
      case "error_if_exists" => "errorifexists"
      // append/overwrite, stay the same
      case "append" | "overwrite" => modeStr
      case others: Any => throw new UnsupportedHybridSeException(s"unsupported write mode $others")
    }

    // only for PhysicalLoadDataNode
    var deepCopy: Option[Boolean] = None
    if (node.isInstanceOf[PhysicalLoadDataNode]) {
      deepCopy = Option(parseOption(getOptionFromNode(node, "deep_copy"), "true", getBoolOrDefault).toBoolean)
    }
    (format, options.toMap, mode, deepCopy)
  }

  // result 'readSchema' & 'tsCols' is only for csv format, may not be used
  def extractOriginAndReadSchema(columns: util.List[Common.ColumnDesc]): (StructType, StructType, List[String]) = {
    var oriSchema = new StructType
    var readSchema = new StructType
    val tsCols = mutable.ArrayBuffer[String]()
    columns.foreach(col => {
      var ty = col.getDataType
      oriSchema = oriSchema.add(col.getName, SparkRowUtil.protoTypeToScalaType(ty), !col
        .getNotNull)
      if (ty.equals(proto.Type.DataType.kTimestamp)) {
        tsCols += col.getName
        // use string to parse ts column, to avoid getting null(parse wrong format), can't distinguish between the
        // parsed null and the real `null`.
        ty = proto.Type.DataType.kString
      }
      readSchema = readSchema.add(col.getName, SparkRowUtil.protoTypeToScalaType(ty), !col
        .getNotNull)
    }
    )
    (oriSchema, readSchema, tsCols.toList)
  }

  def parseLongTsCols(reader: DataFrameReader, readSchema: StructType, tsCols: List[String], file: String)
  : List[String] = {
    val longTsCols = mutable.ArrayBuffer[String]()
    if (tsCols.nonEmpty) {
      // normal timestamp format is TimestampType(Y-M-D H:M:S...)
      // and we support one more timestamp format LongType(ms)
      // read one row to auto detect the format, if int64, use LongType to read file, then convert it to TimestampType
      // P.S. don't use inferSchema, cuz we just need to read the first non-null row, not all
      val df = reader.schema(readSchema).load(file)
      // check timestamp cols
      for (col <- tsCols) {
        val i = readSchema.fieldIndex(col)
        var ty: DataType = LongType
        try {
          // value is string, try to parse to long
          df.select(first(df.col(col), ignoreNulls = true)).first().getString(0).toLong
          longTsCols.append(col)
        } catch {
          case e: Any =>
            logger.debug(s"col '$col' parse long failed, use TimestampType to read", e)
            ty = TimestampType
        }

        val newField = StructField(readSchema.fields(i).name, ty, readSchema.fields(i).nullable)
        readSchema.fields(i) = newField
      }
    }
    longTsCols.toList
  }

  // We want df with oriSchema, but if the file format is csv:
  // 1. we support two format of timestamp
  // 2. spark read may change the df schema to all nullable
  // So we should fix it.
  def autoLoad(spark: SparkSession, file: String, format: String, options: Map[String, String], columns: util
  .List[Common.ColumnDesc]): DataFrame = {
    val reader = spark.read.options(options)
    val (oriSchema, readSchema, tsCols) = HybridseUtil.extractOriginAndReadSchema(columns)
    if (format != "csv") {
      return reader.schema(oriSchema).format(format).load(file)
    }
    // csv should auto detect the timestamp format

    logger.info(s"set file format: $format")
    reader.format(format)
    // use string to read, then infer the format by the first non-null value of the ts column
    val longTsCols = HybridseUtil.parseLongTsCols(reader, readSchema, tsCols, file)
    logger.info(s"read schema: $readSchema, file $file")
    var df = reader.schema(readSchema).load(file)
    if (longTsCols.nonEmpty) {
      // convert long type to timestamp type
      for (tsCol <- longTsCols) {
        df = df.withColumn(tsCol, (col(tsCol) / 1000).cast("timestamp"))
      }
    }

    // if we read non-streaming files, the df schema fields will be set as all nullable.
    // so we need to set it right
    logger.info(s"after read schema: ${df.schema}")
    if (!df.schema.equals(oriSchema)) {
      df = df.sqlContext.createDataFrame(df.rdd, oriSchema)
    }

    require(df.schema == oriSchema, "df schema must == table schema")
    if (logger.isDebugEnabled()) {
      logger.debug("read dataframe count: {}", df.count())
      df.show(10)
    }
    df
  }
}
