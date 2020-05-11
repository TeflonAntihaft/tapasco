/*
 *
 * Copyright (c) 2014-2020 Embedded Systems and Applications, TU Darmstadt.
 *
 * This file is part of TaPaSCo
 * (see https://github.com/esa-tu-darmstadt/tapasco).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */
/**
  * @file DesignSpace.scala
  * @brief    The DesignSpace class models the discrete design space for TPC hardware
  *           designs. Essentially, it provides a list of configurations consisting of
  *           a composition and target frequency, which can then be passed to the
  *           Compose task to generate a bitstream. Compose can automatically iterate
  *           over the design space in case of failure, thus performing a simple design
  *           space exploration (DSE). DesignSpace offers helper methods to define and
  *           span the design space one is interested in. At the moment there are three
  *           basic dimensions: Frequency, Utilization (i.e., number of instances) and
  *           Alternatives. Variation of the frequency is straightforward; variation of
  *           the utilization modifies the number of instances proportionally (keeping
  *           at least one instance of each core) to optimize the area utilization.
  *           Alternatives considers all kernels with the same ID as alternative
  *           implementations of the same computation and generates all compositions
  *           with all available alternatives. Finally, a heuristic is applied to order
  *           the design space; the heuristic function should encode the optimization
  *           goal in a ordering.
  * @authors J. Korinth, TU Darmstadt (jk@esa.cs.tu-darmstadt.de)
  **/
package tapasco.dse

import java.nio.file.Paths

import tapasco.base._
import tapasco.dse.Heuristics._
import tapasco.filemgmt.FileAssetManager
import tapasco.util.LogFormatter._
import tapasco.util.SlotOccupation

class DesignSpace(
                   bd: Composition,
                   target: Target,
                   val heuristic: Heuristic,
                   val dim: DesignSpace.Dimensions = DesignSpace.defaultDim,
                   val designFrequency: Option[Heuristics.Frequency]
                 )(implicit cfg: Configuration) {

  import scala.util.Properties.{lineSeparator => NL}

  private[this] final val DEFAULT_CLOCK_PERIOD_NS = 4 // default: 250 MHz
  private[this] val logger = tapasco.Logging.logger(this.getClass)
  logger.trace(Seq("DesignSpace(", dim, ")") mkString)

  lazy val feasibleFreqs: Seq[Double] = feasibleFreqs(bd)

  private def feasibleFreqs(bd: Composition): Seq[Double] = if (dim.frequency) {
    if(designFrequency.isDefined){
      target.pd.supportedFrequencies.map(_.toDouble).filter(_ <= designFrequency.get).sortWith(_ > _)
    }
    else{
      val cores = bd.composition flatMap (ce => FileAssetManager.entities.core(ce.kernel, target))
      val srs = cores flatMap { c: Core => FileAssetManager.reports.synthReport(c.name, target) }
      val cps = srs flatMap (_.timing) map (_.clockPeriod)
      val fmax = 1000.0 / (if (cps.nonEmpty) cps.max else DEFAULT_CLOCK_PERIOD_NS)
      target.pd.supportedFrequencies map (_.toDouble) filter (_ <= fmax) sortWith (_ > _)
    }
  } else {
    Seq(designFrequency.getOrElse(100.0))
  }

  lazy val feasibleCompositions: Seq[Composition] = compositions(bd)

  /**
    * Computes the set of feasible compositions for a given base composition.
    * The given composition defines the ratios of the kernels, each kernel will be
    * instantiated at least once.
    **/
  private def feasibleCompositions(bd: Composition): Seq[Composition] = if (dim.utilization) {
    val counts = bd.composition map (_.count)
    val minCounts = counts map (n => Seq(java.lang.Math.round(n / counts.min.toDouble).toInt, 1).max)
    val cores = bd.composition flatMap (ce => FileAssetManager.entities.core(ce.kernel, target))
    val srs = cores flatMap { c: Core => FileAssetManager.reports.synthReport(c.name, target) }
    val areaEstimates = srs flatMap (_.area)
    val slotOccupations = srs map (r => SlotOccupation(r.slaves.get, target.pd.slotCount))
    val targetUtil = target.pd.targetUtilization.toDouble
    logger.trace("target util = " + targetUtil)

    def slotUtil(counts: Seq[Int]): SlotOccupation = (slotOccupations zip counts).map(o => o._1 * o._2).reduce(_ + _)
    // check if there is any feasible composition
    if (!slotUtil(minCounts).isFeasible) {
      throw new Exception("Composition infeasible! Exceeds maximal slot count of " + target.pd.slotCount
        + " for " + target + "." + NL + bd)
    }

    def areaUtil(counts: Seq[Int]) = (areaEstimates zip counts) map (a => a._1 * a._2) reduce (_ + _)

    // compute number of steps
    val currUtil = areaUtil(minCounts).utilization
    val df: Int = Seq(java.lang.Math.round((targetUtil - currUtil) / currUtil).toInt, 1).max
    val currSlots = slotUtil(minCounts).slots
    val targetSlots = target.pd.slotCount
    val sf: Int = Math.max(Math.round(targetSlots.toDouble / currSlots.toDouble), 1).toInt
    val steps = Math.min(sf, df)

    logger.trace("minCounts = " + minCounts + " currUtil = " + currUtil)

    // compute feasible sequences as multiples of minCounts
    val seqs = (for (i <- steps to 1 by -1)
      yield minCounts map (n => i * n)) filter (c => areaUtil(c).isFeasible && slotUtil(c).isFeasible)

    logger.trace("number of feasible counts: " + seqs.length)
    if (seqs.isEmpty) {
      logger.warn("No feasible composition found; please check starting composition ratios: " + NL + bd)
    }

    // make sequence of CompositionEntries
    val ces = for {
      s <- seqs filter (_.sum <= target.pd.slotCount)
    } yield bd.composition.map(_.kernel) zip s map (x => Composition.Entry(x._1, x._2))

    // make full composition
    ces map (Composition(Paths.get("N/A"), Some("Generated composition."), _))
  } else {
    Seq(bd)
  }

  private def feasibleAlternatives(bd: Composition): Seq[Composition] =
    if (dim.alternatives) Alternatives.alternatives(bd, target) else Seq(bd)

  private def compositions(bd: Composition): Seq[Composition] =
    feasibleAlternatives(bd) map (feasibleCompositions(_)) reduce (_ ++ _)

  lazy val enumerate: Seq[DesignSpace.Element] = (for {
    bd <- compositions(bd)
    f <- feasibleFreqs(bd)
  } yield DesignSpace.Element(bd, f, heuristic(bd, f, target)(cfg))) sortBy (_.h) reverse

  // TODO remove this and move to proper dumping helper class
  def dumpC(bd: Composition, sep: String = "; "): String = bd.composition.map(ce => Seq(ce.kernel, " x ", ce.count).mkString).mkString(sep)

  def dump(sep: String = ",", header: Boolean = true): String =
    (if (header) Seq("Composition", "Frequency", "H").mkString(sep) + NL else "") +
      (enumerate map (p => Seq(dumpC(p.composition), p.frequency, p.h) mkString sep)).mkString(NL)
}

object DesignSpace {

  final case class Dimensions(
                               frequency: Boolean = false,
                               utilization: Boolean = false,
                               alternatives: Boolean = false) {
    override lazy val toString: String = "(%s)".format(
      Seq(("freq", frequency), ("area", utilization), ("alternatives", alternatives))
        .filter(_._2) map (_._1) mkString ", ")
  }

  final case class Element(
                            composition: Composition,
                            frequency: Heuristics.Frequency,
                            h: Heuristics.Value
                          ) {
    override lazy val toString: String = "Element[Composition: %s, Freq: %3.2f, H: %3.2f]"
      .format(logformat(composition), frequency, h)
  }

  val defaultDim = Dimensions(true, false, false)

}
