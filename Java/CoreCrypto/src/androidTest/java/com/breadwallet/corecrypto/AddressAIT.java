package com.breadwallet.corecrypto;

import com.google.common.base.Optional;
import com.google.common.primitives.UnsignedInteger;
import com.google.common.primitives.UnsignedLong;

import org.junit.Test;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;

import static org.junit.Assert.*;

public class AddressAIT {

    @Test
    public void testAddressCreateAsEthOnMainnet() {
        Currency eth = Currency.create("Ethereum", "Ethereum", "eth", "native", null);

        Unit wei_eth = Unit.create(eth, "ETH-WEI", "WEI", "wei");
        Unit gwei_eth = Unit.create(eth, "ETH-GWEI", "GWEI",  "gwei", wei_eth, UnsignedInteger.valueOf(9));
        Unit ether_eth = Unit.create(eth, "ETH-ETH", "ETHER", "E",    wei_eth, UnsignedInteger.valueOf(18));

        NetworkAssociation association_eth = new NetworkAssociation(wei_eth, ether_eth, new HashSet<>(Arrays.asList(wei_eth, gwei_eth, ether_eth)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(eth, association_eth);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(1000), Amount.create(2.0, gwei_eth).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network = Network.create("ethereum-mainnet", "Ethereum", true, eth, UnsignedLong.valueOf(100000), associations, fees);

        Optional<Address> oe1 = network.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62");
        assertTrue(oe1.isPresent());
        Address e1 = oe1.get();
        assertEquals("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62", e1.toString());

        Optional<Address> oe2 = network.addressFor("0xd3CFBA03Fc13dc01F0C67B88CBEbE776D8F3DE8f");
        assertTrue(oe2.isPresent());
        Address e2 = oe2.get();
        assertEquals("0xd3CFBA03Fc13dc01F0C67B88CBEbE776D8F3DE8f", e2.toString());

        Optional<Address> oe3 = network.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62");
        assertTrue(oe3.isPresent());
        Address e3 = oe3.get();
        assertEquals("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62", e3.toString());

        assertEquals(e1, e1);
        assertEquals(e1, e3);
        assertEquals(e3, e1);

        assertNotEquals(e1, e2);
        assertNotEquals(e2, e1);
    }

    @Test
    public void testAddressCreateAsBtcOnMainnet() {
        Currency btc = Currency.create("Bitcoin", "Bitcoin", "btc", "native", null);

        Unit satoshi_btc = Unit.create(btc, "BTC-SAT", "Satoshi", "SAT");
        Unit btc_btc = Unit.create(btc, "BTC-BTC", "Bitcoin", "B", satoshi_btc, UnsignedInteger.valueOf(8));

        NetworkAssociation association = new NetworkAssociation(satoshi_btc, btc_btc, new HashSet<>(Arrays.asList(satoshi_btc, btc_btc)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(btc, association);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(30 * 1000), Amount.create(1000, satoshi_btc).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network = Network.create("bitcoin-mainnet", "Bitcoin", true, btc, UnsignedLong.valueOf(100000), associations, fees);

        Optional<Address> ob1 = network.addressFor("1CC3X2gu58d6wXUWMffpuzN9JAfTUWu4Kj");
        assertTrue(ob1.isPresent());
        Address b1 = ob1.get();
        assertEquals("1CC3X2gu58d6wXUWMffpuzN9JAfTUWu4Kj", b1.toString());

        // TODO: Expand coverage
    }

    @Test
    public void testAddressMixedEqualsOnMainnet() {
        //
        // BTC
        //

        Currency btc = Currency.create("Bitcoin", "Bitcoin", "btc", "native", null);

        Unit satoshi_btc = Unit.create(btc, "BTC-SAT", "Satoshi", "SAT");
        Unit btc_btc = Unit.create(btc, "BTC-BTC", "Bitcoin", "B", satoshi_btc, UnsignedInteger.valueOf(8));

        NetworkAssociation association = new NetworkAssociation(satoshi_btc, btc_btc, new HashSet<>(Arrays.asList(satoshi_btc, btc_btc)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(btc, association);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(30 * 1000), Amount.create(1000, satoshi_btc).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network_btc = Network.create("bitcoin-mainnet", "Bitcoin", true, btc, UnsignedLong.valueOf(100000), associations, fees);

        //
        // ETH
        //

        Currency eth = Currency.create("Ethereum", "Ethereum", "eth", "native", null);

        Unit wei_eth = Unit.create(eth, "ETH-WEI", "WEI", "wei");
        Unit gwei_eth = Unit.create(eth, "ETH-GWEI", "GWEI",  "gwei", wei_eth, UnsignedInteger.valueOf(9));
        Unit ether_eth = Unit.create(eth, "ETH-ETH", "ETHER", "E",    wei_eth, UnsignedInteger.valueOf(18));

        NetworkAssociation association_eth = new NetworkAssociation(wei_eth, ether_eth, new HashSet<>(Arrays.asList(wei_eth, gwei_eth, ether_eth)));

        associations = new HashMap<>();
        associations.put(eth, association_eth);

        fee = NetworkFee.create(UnsignedLong.valueOf(1000), Amount.create(2.0, gwei_eth).get());
        fees = Collections.singletonList(fee);

        Network network_eth = Network.create("ethereum-mainnet", "Ethereum", true, eth, UnsignedLong.valueOf(100000), associations, fees);

        Address e1 = network_eth.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62").get();
        Address b1 = network_btc.addressFor("1CC3X2gu58d6wXUWMffpuzN9JAfTUWu4Kj").get();

        assertNotEquals(e1, b1);
        assertNotEquals(b1, e1);
    }

    @Test
    public void testAddressCreateAsEthOnTestnet() {
        Currency eth = Currency.create("Ethereum", "Ethereum", "eth", "native", null);

        Unit wei_eth = Unit.create(eth, "ETH-WEI", "WEI", "wei");
        Unit gwei_eth = Unit.create(eth, "ETH-GWEI", "GWEI",  "gwei", wei_eth, UnsignedInteger.valueOf(9));
        Unit ether_eth = Unit.create(eth, "ETH-ETH", "ETHER", "E",    wei_eth, UnsignedInteger.valueOf(18));

        NetworkAssociation association_eth = new NetworkAssociation(wei_eth, ether_eth, new HashSet<>(Arrays.asList(wei_eth, gwei_eth, ether_eth)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(eth, association_eth);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(1000), Amount.create(2.0, gwei_eth).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network = Network.create("ethereum-testnet", "Ethereum Testnet", false, eth, UnsignedLong.valueOf(100000), associations, fees);

        Optional<Address> oe1 = network.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62");
        assertTrue(oe1.isPresent());
        Address e1 = oe1.get();
        assertEquals("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62", e1.toString());

        Optional<Address> oe2 = network.addressFor("0xd3CFBA03Fc13dc01F0C67B88CBEbE776D8F3DE8f");
        assertTrue(oe2.isPresent());
        Address e2 = oe2.get();
        assertEquals("0xd3CFBA03Fc13dc01F0C67B88CBEbE776D8F3DE8f", e2.toString());

        Optional<Address> oe3 = network.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62");
        assertTrue(oe3.isPresent());
        Address e3 = oe3.get();
        assertEquals("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62", e3.toString());

        assertEquals(e1, e1);
        assertEquals(e1, e3);
        assertEquals(e3, e1);

        assertNotEquals(e1, e2);
        assertNotEquals(e2, e1);
    }

    @Test
    public void testAddressCreateAsBtcOnTestnet() {
        Currency btc = Currency.create("Bitcoin", "Bitcoin", "btc", "native", null);

        Unit satoshi_btc = Unit.create(btc, "BTC-SAT", "Satoshi", "SAT");
        Unit btc_btc = Unit.create(btc, "BTC-BTC", "Bitcoin", "B", satoshi_btc, UnsignedInteger.valueOf(8));

        NetworkAssociation association = new NetworkAssociation(satoshi_btc, btc_btc, new HashSet<>(Arrays.asList(satoshi_btc, btc_btc)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(btc, association);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(30 * 1000), Amount.create(1000, satoshi_btc).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network = Network.create("bitcoin-testnet", "Bitcoin Testnet", false, btc, UnsignedLong.valueOf(100000), associations, fees);

        Optional<Address> ob1 = network.addressFor("mm7DDqVkFd35XcWecFipfTYM5dByBzn7nq");
        assertTrue(ob1.isPresent());
        Address b1 = ob1.get();
        assertEquals("mm7DDqVkFd35XcWecFipfTYM5dByBzn7nq", b1.toString());

        // TODO: Expand coverage
    }

    @Test
    public void testAddressMixedEqualsOnTestnet() {
        //
        // BTC
        //

        Currency btc = Currency.create("Bitcoin", "Bitcoin", "btc", "native", null);

        Unit satoshi_btc = Unit.create(btc, "BTC-SAT", "Satoshi", "SAT");
        Unit btc_btc = Unit.create(btc, "BTC-BTC", "Bitcoin", "B", satoshi_btc, UnsignedInteger.valueOf(8));

        NetworkAssociation association = new NetworkAssociation(satoshi_btc, btc_btc, new HashSet<>(Arrays.asList(satoshi_btc, btc_btc)));

        Map<Currency, NetworkAssociation> associations = new HashMap<>();
        associations.put(btc, association);

        NetworkFee fee = NetworkFee.create(UnsignedLong.valueOf(30 * 1000), Amount.create(1000, satoshi_btc).get());
        List<NetworkFee> fees = Collections.singletonList(fee);

        Network network_btc = Network.create("bitcoin-testnet", "Bitcoin Testnet", false, btc, UnsignedLong.valueOf(100000), associations, fees);

        //
        // ETH
        //

        Currency eth = Currency.create("Ethereum", "Ethereum", "eth", "native", null);

        Unit wei_eth = Unit.create(eth, "ETH-WEI", "WEI", "wei");
        Unit gwei_eth = Unit.create(eth, "ETH-GWEI", "GWEI",  "gwei", wei_eth, UnsignedInteger.valueOf(9));
        Unit ether_eth = Unit.create(eth, "ETH-ETH", "ETHER", "E",    wei_eth, UnsignedInteger.valueOf(18));

        NetworkAssociation association_eth = new NetworkAssociation(wei_eth, ether_eth, new HashSet<>(Arrays.asList(wei_eth, gwei_eth, ether_eth)));

        associations = new HashMap<>();
        associations.put(eth, association_eth);

        fee = NetworkFee.create(UnsignedLong.valueOf(1000), Amount.create(2.0, gwei_eth).get());
        fees = Collections.singletonList(fee);

        Network network_eth = Network.create("ethereum-testnet", "Ethereum Testnet", false, eth, UnsignedLong.valueOf(100000), associations, fees);

        Address e1 = network_eth.addressFor("0xb0F225defEc7625C6B5E43126bdDE398bD90eF62").get();
        Address b1 = network_btc.addressFor("mm7DDqVkFd35XcWecFipfTYM5dByBzn7nq").get();

        assertNotEquals(e1, b1);
        assertNotEquals(b1, e1);
    }
}